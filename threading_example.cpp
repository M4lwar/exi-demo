// threading_example.cpp — the correct GraalVM isolate/threading pattern for
// integrating libexificient into a threaded C++ application.
//
// It demonstrates two things that trip up first-time integrators:
//
//   1. Hiding the seconds-long schema load off the main startup path.
//      graal_create_isolate is cheap (stands up the GC heap, ~ms) and runs on
//      the main thread up front. exi_create is the expensive part (XSD parse +
//      grammar build, seconds for a large schema) -- so ONLY that is pushed to
//      a background thread via std::async, and main proceeds immediately.
//
//   2. The per-thread token rule. A graal_isolatethread_t* is valid ONLY on
//      the thread that obtained it; it must never cross threads. Every thread
//      (the loader AND the workers) calls graal_attach_thread to get its own
//      token and graal_detach_thread when done. The exi_ctx, by contrast, is a
//      global isolate-wide handle -- create it once, share it across every
//      thread (it outlives the thread that created it).
//
// Build: added as a second target in CMakeLists.txt; run from the repo root so
// the schema and sample paths resolve. This links the same prebuilt exificient
// Conan package the demo uses.

#include "exificient.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// A large real schema so exi_create genuinely takes seconds -- the latency this
// pattern hides. Explicit schema paths always runtime-load, even on a build
// with a baked schema, so this exercises the slow path on purpose.
constexpr const char* SCHEMA = "schemas/UCI_MessageDefinitions_v2_5_0.xsd";
constexpr const char* SAMPLE = "samples/entity.xml";

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

std::string last_error(graal_isolatethread_t* t) {
    char buf[512];
    size_t n = exi_last_error(t, buf, sizeof buf);
    return std::string(buf, n < sizeof buf ? n : sizeof buf - 1);
}

std::vector<char> read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(n > 0 ? static_cast<size_t>(n) : 0);
    if (n > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
    std::fclose(f);
    return buf;
}

}  // namespace

int main() {
    // --- Main thread, up front: create the isolate (cheap). Its token is only
    //     valid here; other threads will attach for their own. ---
    graal_isolate_t* isolate = nullptr;
    graal_isolatethread_t* main_t = nullptr;
    const auto t_iso = std::chrono::steady_clock::now();
    if (graal_create_isolate(nullptr, &isolate, &main_t) != 0) {
        std::fprintf(stderr, "graal_create_isolate failed\n");
        return 1;
    }
    std::printf("isolate created in %.1f ms (main thread continues)\n",
                ms_since(t_iso));

    // --- Hide the SLOW exi_create behind a background thread. Capture BY VALUE
    //     (isolate handle + schema string): this task outlives the current
    //     scope, so a [&] capture-by-reference would dangle -- a real source of
    //     heap corruption. std::async also propagates load failures cleanly. ---
    const auto t_kick = std::chrono::steady_clock::now();
    std::future<exi_ctx> ctx_future = std::async(
        std::launch::async,
        [isolate, schema = std::string(SCHEMA)]() -> exi_ctx {
            graal_isolatethread_t* wt = nullptr;   // this thread's OWN token
            if (graal_attach_thread(isolate, &wt) != 0)
                throw std::runtime_error("graal_attach_thread (loader) failed");
            exi_ctx ctx = nullptr;
            exi_status s = exi_create(wt, schema.c_str(), 0, &ctx);
            std::string err = (s == EXI_OK) ? "" : last_error(wt);
            graal_detach_thread(wt);               // ctx SURVIVES this detach
            if (s != EXI_OK)
                throw std::runtime_error("exi_create failed: " + err);
            return ctx;                            // valid on ANY attached thread
        });

    // --- Main thread does the rest of application startup here, unblocked. ---
    std::printf("main: doing other startup work while the schema loads...\n");

    // --- When the context is first actually needed, wait for it (blocks only
    //     if the load hasn't finished yet). ---
    exi_ctx ctx = nullptr;
    try {
        ctx = ctx_future.get();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "context load failed: %s\n", e.what());
        return 1;
    }
    std::printf("context ready after %.1f ms (schema parse + grammar build)\n",
                ms_since(t_kick));

    // --- Now hammer the ONE shared context from several worker threads. Each
    //     worker attaches for its own token; the context is shared and
    //     immutable, so concurrent encodes are safe. ---
    std::vector<char> xml = read_file(SAMPLE);
    if (xml.empty()) {
        std::fprintf(stderr, "cannot read %s (run from the repo root)\n", SAMPLE);
        return 1;
    }

    constexpr int NTHREADS = 4;
    constexpr long ITERS = 500;
    std::atomic<long> ok{0}, failed{0};

    std::vector<std::thread> workers;
    for (int i = 0; i < NTHREADS; ++i) {
        workers.emplace_back([&] {
            graal_isolatethread_t* wt = nullptr;   // this worker's OWN token
            if (graal_attach_thread(isolate, &wt) != 0) { ++failed; return; }
            for (long k = 0; k < ITERS; ++k) {
                char* out = nullptr;
                size_t out_len = 0;
                if (exi_encode(wt, ctx, xml.data(), xml.size(), &out, &out_len)
                        == EXI_OK) {
                    exi_free(wt, out);             // free with THIS thread's token
                    ++ok;
                } else {
                    ++failed;
                }
            }
            graal_detach_thread(wt);
        });
    }
    for (auto& w : workers) w.join();              // MUST join before the [&]
                                                   // captures (xml, ctx, ...)
                                                   // go out of scope.

    std::printf("%d threads x %ld encodes on one shared ctx: %ld ok, %ld failed\n",
                NTHREADS, ITERS, ok.load(), failed.load());

    // --- Teardown. Destroy the context from any attached thread (main is still
    //     attached via main_t). The isolate is intentionally NOT torn down: a
    //     long-lived process keeps it for the lifetime of the app. ---
    exi_destroy(main_t, ctx);
    return failed.load() == 0 ? 0 : 1;
}
