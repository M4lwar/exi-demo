// exi-demo: capability showcase for libexificient's v2 C API.
//
//   exi-demo bench   [-s schema] [-n iters] [path]   compression ratio + timing
//   exi-demo peek    [-s schema] [path]              message-type dispatch demo
//   exi-demo headers [-s schema] [file.xml]          $EXI cookie on the wire
//   exi-demo errors  [-s schema]                     status codes + exi_last_error
//   exi-demo threads [-s schema] [-t N] [-n iters]   shared-context concurrency
//
// path: an .xml file or a directory searched recursively (default: samples/).
// The schema (default ./schemas/UCI_MessageDefinitions_v2_5_0.xsd) and any
// schemas it imports must exist on disk; the library bundles none.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "exificient.h"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

static const char* g_schema = "./schemas/UCI_MessageDefinitions_v2_5_0.xsd";

static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static std::vector<char> read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(n > 0 ? static_cast<size_t>(n) : 0);
    if (n > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
    std::fclose(f);
    return buf;
}

static std::vector<std::string> collect_messages(const std::string& path) {
    std::vector<std::string> files;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(path, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".xml")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(path);
    }
    return files;
}

static void die(graal_isolatethread_t* thread, const char* what) {
    char err[512] = {0};
    if (thread) exi_last_error(thread, err, sizeof err);
    fprintf(stderr, "Error: %s%s%s\n", what, err[0] ? ": " : "", err);
    exit(1);
}

static exi_ctx make_ctx(graal_isolatethread_t* thread, const char* schema, uint32_t flags) {
    exi_ctx ctx = nullptr;
    if (exi_create(thread, schema, flags, &ctx) != EXI_OK)
        die(thread, "exi_create failed (is the schema present?)");
    return ctx;
}

// ---------------------------------------------------------------- bench ----

static int cmd_bench(graal_isolatethread_t* thread, const std::string& path, long iterations) {
    const auto t0 = clk::now();
    exi_ctx ctx = make_ctx(thread, g_schema, 0);
    printf("\n  Schema     : %s\n", g_schema);
    printf("  exi_create : %.3f ms  (one-time: schema load + grammar build)\n\n", ms_since(t0));

    std::vector<std::string> messages = collect_messages(path);
    if (messages.empty()) { fprintf(stderr, "Error: no .xml at '%s'\n", path.c_str()); exi_destroy(thread, ctx); return 1; }

    printf("  %-40s %9s %9s %8s", "message", "XML", "EXI", "saved");
    if (iterations > 1) printf("  %10s %10s", "enc avg ms", "dec avg ms");
    printf("\n  %s\n", std::string(iterations > 1 ? 92 : 69, '-').c_str());

    long total_xml = 0, total_exi = 0, ok = 0;
    for (const auto& msg : messages) {
        std::vector<char> xml = read_file(msg);
        std::string name = fs::path(msg).lexically_normal().string();
        if (name.size() > 40) name = "..." + name.substr(name.size() - 37);
        if (xml.empty()) { printf("  %-40s %9s\n", name.c_str(), "(unreadable)"); continue; }

        size_t exi_len = 0;
        double enc_sum = 0, dec_sum = 0;
        bool failed = false;
        for (long i = 0; i < iterations && !failed; ++i) {
            char* exi = nullptr;
            const auto e0 = clk::now();
            if (exi_encode(thread, ctx, xml.data(), xml.size(), &exi, &exi_len) != EXI_OK) { failed = true; break; }
            enc_sum += ms_since(e0);
            char* out = nullptr; size_t out_len = 0;
            const auto d0 = clk::now();
            if (exi_decode(thread, ctx, exi, exi_len, &out, &out_len) != EXI_OK) { exi_free(thread, exi); failed = true; break; }
            dec_sum += ms_since(d0);
            exi_free(thread, exi);
            exi_free(thread, out);
        }
        if (failed) { printf("  %-40s %9zu %9s\n", name.c_str(), xml.size(), "FAILED"); continue; }

        const double saved = xml.empty() ? 0.0 : (1.0 - double(exi_len) / xml.size()) * 100.0;
        printf("  %-40s %9zu %9zu %7.1f%%", name.c_str(), xml.size(), exi_len, saved);
        if (iterations > 1) printf("  %10.3f %10.3f", enc_sum / iterations, dec_sum / iterations);
        printf("\n");
        total_xml += long(xml.size()); total_exi += long(exi_len); ++ok;
    }
    printf("  %s\n", std::string(iterations > 1 ? 92 : 69, '-').c_str());
    printf("  %-40s %9ld %9ld %7.1f%%\n\n", (std::to_string(ok) + " message(s)").c_str(),
           total_xml, total_exi, total_xml ? (1.0 - double(total_exi) / total_xml) * 100.0 : 0.0);
    exi_destroy(thread, ctx);
    return ok > 0 ? 0 : 1;
}

// ---------------------------------------------------------------- peek ----

// Simulates the bus-consumer pattern: identify each message's UCI type from
// the first bytes of its EXI form (no full decode), then "dispatch" it.
static int cmd_peek(graal_isolatethread_t* thread, const std::string& path) {
    exi_ctx ctx = make_ctx(thread, g_schema, 0);
    std::vector<std::string> messages = collect_messages(path);
    if (messages.empty()) { fprintf(stderr, "Error: no .xml at '%s'\n", path.c_str()); exi_destroy(thread, ctx); return 1; }

    printf("\n  %-40s %-28s %12s %12s\n", "message", "peeked type", "peek ms", "decode ms");
    printf("  %s\n", std::string(96, '-').c_str());
    int ok = 0;
    for (const auto& msg : messages) {
        std::vector<char> xml = read_file(msg);
        if (xml.empty()) continue;
        char* exi = nullptr; size_t exi_len = 0;
        if (exi_encode(thread, ctx, xml.data(), xml.size(), &exi, &exi_len) != EXI_OK) continue;

        char type[256]; size_t type_len = 0;
        const auto p0 = clk::now();
        exi_status ps = exi_peek_root(thread, ctx, exi, exi_len, type, sizeof type, &type_len);
        const double peek_ms = ms_since(p0);

        const auto d0 = clk::now();
        char* out = nullptr; size_t out_len = 0;
        exi_status ds = exi_decode(thread, ctx, exi, exi_len, &out, &out_len);
        const double dec_ms = ms_since(d0);
        if (ds == EXI_OK) exi_free(thread, out);

        std::string name = fs::path(msg).filename().string();
        if (ps == EXI_OK) {
            // dispatch stub: a real consumer looks `type` up in a serializer registry
            printf("  %-40s %-28s %12.3f %12.3f\n", name.c_str(), type, peek_ms, dec_ms);
            ++ok;
        } else {
            printf("  %-40s %-28s\n", name.c_str(), "(peek failed)");
        }
        exi_free(thread, exi);
    }
    printf("\n  peek reads only the stream head — cost is independent of message size.\n\n");
    exi_destroy(thread, ctx);
    return ok > 0 ? 0 : 1;
}

// ---------------------------------------------------------------- headers ----

static void hexdump16(const char* label, const char* p, size_t n) {
    printf("  %-14s", label);
    for (size_t i = 0; i < 16 && i < n; ++i) printf(" %02x", (unsigned char)p[i]);
    printf("   |");
    for (size_t i = 0; i < 16 && i < n; ++i) printf("%c", isprint((unsigned char)p[i]) ? p[i] : '.');
    printf("|\n");
}

// Shows the wire-format difference EXI_HEADER_COOKIE makes: the 4-byte "$EXI"
// magic that lets a bus reader identify EXI payloads by sniffing.
static int cmd_headers(graal_isolatethread_t* thread, const std::string& path) {
    std::vector<std::string> messages = collect_messages(path);
    if (messages.empty()) { fprintf(stderr, "Error: no .xml at '%s'\n", path.c_str()); return 1; }
    std::vector<char> xml = read_file(messages.front());
    if (xml.empty()) { fprintf(stderr, "Error: unreadable %s\n", messages.front().c_str()); return 1; }

    exi_ctx plain = make_ctx(thread, g_schema, 0);
    exi_ctx cookie = make_ctx(thread, g_schema, EXI_HEADER_COOKIE);

    char* e1 = nullptr; size_t n1 = 0;
    char* e2 = nullptr; size_t n2 = 0;
    if (exi_encode(thread, plain, xml.data(), xml.size(), &e1, &n1) != EXI_OK ||
        exi_encode(thread, cookie, xml.data(), xml.size(), &e2, &n2) != EXI_OK)
        die(thread, "encode failed");

    printf("\n  message: %s (%zu bytes XML)\n\n", messages.front().c_str(), xml.size());
    hexdump16("default:", e1, n1);
    hexdump16("with cookie:", e2, n2);
    printf("\n  cookie stream starts with \"$EXI\": %s;  size cost: %zu bytes (%zu -> %zu)\n",
           (n2 >= 4 && memcmp(e2, "$EXI", 4) == 0) ? "yes" : "NO (?)", n2 - n1, n1, n2);

    // both decode with the same context: the decoder skips the cookie itself
    char* out = nullptr; size_t out_len = 0;
    printf("  cookie stream decodes via plain ctx: %s\n\n",
           exi_decode(thread, plain, e2, n2, &out, &out_len) == EXI_OK ? "yes" : "no");
    if (out) exi_free(thread, out);
    exi_free(thread, e1); exi_free(thread, e2);
    exi_destroy(thread, plain); exi_destroy(thread, cookie);
    return 0;
}

// ---------------------------------------------------------------- errors ----

static const char* status_name(exi_status s) {
    switch (s) {
        case EXI_OK: return "EXI_OK";
        case EXI_ERR_INVALID_ARG: return "EXI_ERR_INVALID_ARG";
        case EXI_ERR_SCHEMA_LOAD: return "EXI_ERR_SCHEMA_LOAD";
        case EXI_ERR_INVALID_CONTEXT: return "EXI_ERR_INVALID_CONTEXT";
        case EXI_ERR_MALFORMED_XML: return "EXI_ERR_MALFORMED_XML";
        case EXI_ERR_MALFORMED_EXI: return "EXI_ERR_MALFORMED_EXI";
        case EXI_ERR_BUFFER_TOO_SMALL: return "EXI_ERR_BUFFER_TOO_SMALL";
        case EXI_ERR_INTERNAL: return "EXI_ERR_INTERNAL";
    }
    return "?";
}

static void show(graal_isolatethread_t* thread, const char* what, exi_status s) {
    char err[256] = {0};
    exi_last_error(thread, err, sizeof err);
    printf("  %-38s -> %-26s %s\n", what, status_name(s), s == EXI_OK ? "" : err);
}

// Walks the failure modes a bus consumer meets in practice and shows how each
// is reported: a distinct status code plus a per-thread diagnostic message.
static int cmd_errors(graal_isolatethread_t* thread) {
    printf("\n");
    exi_ctx ctx = nullptr;
    show(thread, "create(missing schema xsd)",
         exi_create(thread, "no/such/schema.xsd", 0, &ctx));
    show(thread, "create(unknown flag bit 5)",
         exi_create(thread, g_schema, 1u << 5, &ctx));

    ctx = make_ctx(thread, g_schema, 0);
    char* out = nullptr; size_t out_len = 0;
    show(thread, "encode(\"this is not xml\")",
         exi_encode(thread, ctx, "this is not xml", 15, &out, &out_len));
    show(thread, "decode(garbage bytes)",
         exi_decode(thread, ctx, "\x7f\x00!DU", 5, &out, &out_len));
    char tiny[3]; size_t need = 0;
    // valid EXI, but a 3-byte name buffer: BUFFER_TOO_SMALL reports the need
    std::vector<char> xml = read_file("samples/position-report.xml");
    char* exi = nullptr; size_t exi_len = 0;
    if (!xml.empty() && exi_encode(thread, ctx, xml.data(), xml.size(), &exi, &exi_len) == EXI_OK) {
        exi_status s = exi_peek_root(thread, ctx, exi, exi_len, tiny, sizeof tiny, &need);
        char what[64];
        snprintf(what, sizeof what, "peek(name_cap=3) [needs %zu]", need);
        show(thread, what, s);
        exi_free(thread, exi);
    }
    show(thread, "decode(NULL ctx)",
         exi_decode(thread, nullptr, "\x00", 1, &out, &out_len));
    exi_destroy(thread, ctx);
    show(thread, "encode(destroyed ctx)",
         exi_encode(thread, ctx, "x", 1, &out, &out_len));
    printf("\n  every failure: a status code now, a message via exi_last_error(thread,...).\n\n");
    return 0;
}

// ------------------------------------------------------- stub (T6) -----

static int cmd_threads(graal_isolatethread_t*, int, long)         { fprintf(stderr, "threads: not implemented\n");return 3; }

// ----------------------------------------------------------------- main ----

static void usage(const char* prog) {
    printf("exi-demo - capability showcase for libexificient (v2 C API)\n\n"
           "Usage: %s <bench|peek|headers|errors|threads> [options] [path]\n\n"
           "Options:\n"
           "  -s, --schema <path>    XSD passed to exi_create (default: %s)\n"
           "  -n, --iterations <N>   bench/threads: iterations per message (default: 1)\n"
           "  -t, --threads <N>      threads: worker count (default: 4)\n"
           "  -h, --help             this help\n", prog, g_schema);
}

int main(int argc, char** argv) {
    std::string sub = argc > 1 ? argv[1] : "";
    if (sub.empty() || sub == "-h" || sub == "--help") { usage(argv[0]); return sub.empty() ? 2 : 0; }

    std::string path = "samples/";
    long iterations = 1;
    int nthreads = 4;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "Error: %s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "-s" || a == "--schema") g_schema = val();
        else if (a == "-n" || a == "--iterations") iterations = std::max(1L, strtol(val(), nullptr, 10));
        else if (a == "-t" || a == "--threads") nthreads = std::max(1, int(strtol(val(), nullptr, 10)));
        else if (a[0] != '-') path = a;
        else { fprintf(stderr, "Error: unknown option '%s'\n", a.c_str()); usage(argv[0]); return 2; }
    }

    graal_isolate_t* isolate = nullptr;
    graal_isolatethread_t* thread = nullptr;
    if (graal_create_isolate(nullptr, &isolate, &thread) != 0) {
        fprintf(stderr, "Error: failed to create GraalVM isolate\n"); return 1;
    }
    if (exi_lib_version(thread) != EXI_API_VERSION)
        fprintf(stderr, "Warning: header %#x vs library %#x version mismatch\n",
                EXI_API_VERSION, exi_lib_version(thread));

    int rc;
    if      (sub == "bench")   rc = cmd_bench(thread, path, iterations);
    else if (sub == "peek")    rc = cmd_peek(thread, path);
    else if (sub == "headers") rc = cmd_headers(thread, path);
    else if (sub == "errors")  rc = cmd_errors(thread);
    else if (sub == "threads") rc = cmd_threads(thread, nthreads, iterations);
    else { fprintf(stderr, "Error: unknown subcommand '%s'\n\n", sub.c_str()); usage(argv[0]); rc = 2; }

    graal_tear_down_isolate(thread);
    return rc;
}
