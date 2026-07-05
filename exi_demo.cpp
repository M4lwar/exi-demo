// exi-demo: capability showcase for libexificient's v2 C API.
//
//   exi-demo bench        [-s schema] [-n iters] [path]   compression ratio + timing
//   exi-demo peek         [-s schema] [path]              message-type dispatch demo
//   exi-demo headers      [-s schema] [file.xml]          $EXI cookie on the wire
//   exi-demo errors       [-s schema]                     status codes + exi_last_error
//   exi-demo threads      [-s schema] [-t N] [-n iters]   shared-context concurrency
//   exi-demo create-cost  [-s schema]                     baked vs runtime exi_create cost
//   exi-demo options <options.xml> [samples-dir]          encode under a W3C EXI options
//                                                          document; compare vs defaults
//   exi-demo uuid <v3|v5> <namespace> <name...>           RFC 4122 name-based ids (no schema)
//
// path: an .xml file or a directory searched recursively (default: samples/).
// The schema (default ./schemas/UCI_MessageDefinitions_v2_5_0.xsd) and any
// schemas it imports must exist on disk; the library bundles none. If the
// linked library reports a baked schema (see exi_baked_schema) and -s was
// not passed, every subcommand uses the baked context (NULL schema) instead.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "exificient.h"
#include "uuid_hash.hpp"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

static const char* g_schema = "./schemas/UCI_MessageDefinitions_v2_5_0.xsd";
static bool g_schema_overridden = false;   // set when -s is passed

// Returns the baked schema id ("" if the linked library is generic).
static std::string baked_id(graal_isolatethread_t* thread) {
    char buf[64];
    exi_baked_schema(thread, buf, sizeof buf);
    return buf;
}

// Baked-aware schema selection: no -s + baked library -> NULL (baked grammars,
// ~instant); otherwise the explicit path (~seconds for UCI).
static const char* effective_schema(graal_isolatethread_t* thread) {
    if (!g_schema_overridden && !baked_id(thread).empty()) return nullptr;
    return g_schema;
}

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
    const char* schema = effective_schema(thread);
    const auto t0 = clk::now();
    exi_ctx ctx = make_ctx(thread, schema, 0);
    if (schema) printf("\n  Schema     : %s\n", schema);
    else printf("\n  Schema     : (baked: %s)\n", baked_id(thread).c_str());
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
    exi_ctx ctx = make_ctx(thread, effective_schema(thread), 0);
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

    exi_ctx plain = make_ctx(thread, effective_schema(thread), 0);
    exi_ctx cookie = make_ctx(thread, effective_schema(thread), EXI_HEADER_COOKIE);

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
    // bit 5 used to be unassigned; v1.0.0 promoted it to EXI_OPT_COMPRESSION
    // (bit 1 is now EXI_OPT_OPTIONS_IN_HEADER), so it's a valid flag today -
    // bit 15 is still genuinely unknown.
    show(thread, "create(unknown flag bit 15)",
         exi_create(thread, effective_schema(thread), 1u << 15, &ctx));

    ctx = make_ctx(thread, effective_schema(thread), 0);
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

// ---------------------------------------------------------------- threads ----

// Demonstrates the documented thread-safety contract: one context, many
// attached threads, byte-identical results, near-linear encode throughput.
static int cmd_threads(graal_isolate_t* isolate, graal_isolatethread_t* thread,
                       int nthreads, long iterations) {
    if (iterations < 50) iterations = 50;
    std::vector<char> xml = read_file("samples/position-report.xml");
    if (xml.empty()) { fprintf(stderr, "Error: samples/position-report.xml unreadable\n"); return 1; }
    exi_ctx ctx = make_ctx(thread, effective_schema(thread), 0);

    char* ref = nullptr; size_t ref_len = 0;
    if (exi_encode(thread, ctx, xml.data(), xml.size(), &ref, &ref_len) != EXI_OK)
        die(thread, "reference encode failed");
    const std::string reference(ref, ref_len);
    exi_free(thread, ref);

    std::atomic<long> done{0}, mismatches{0}, failures{0};
    const auto t0 = clk::now();
    std::vector<std::thread> workers;
    for (int t = 0; t < nthreads; ++t) {
        workers.emplace_back([&] {
            graal_isolatethread_t* wt = nullptr;
            if (graal_attach_thread(isolate, &wt) != 0) { ++failures; return; }
            for (long i = 0; i < iterations; ++i) {
                char* e = nullptr; size_t el = 0;
                if (exi_encode(wt, ctx, xml.data(), xml.size(), &e, &el) != EXI_OK) { ++failures; continue; }
                if (std::string(e, el) != reference) ++mismatches;
                exi_free(wt, e);
                ++done;
            }
            graal_detach_thread(wt);
        });
    }
    for (auto& w : workers) w.join();
    const double secs = ms_since(t0) / 1000.0;

    printf("\n  %d thread(s) x %ld iterations on ONE shared exi_ctx\n", nthreads, iterations);
    printf("  encodes:    %ld ok, %ld failed, %ld byte-mismatches\n",
           done.load(), failures.load(), mismatches.load());
    printf("  throughput: %.0f encodes/s (%.3f s total)\n\n", done / secs, secs);
    exi_destroy(thread, ctx);
    return (failures == 0 && mismatches == 0) ? 0 : 1;
}

// ------------------------------------------------------------ create-cost ----

// Shows why baking exists: creation cost of the baked context vs a full
// runtime schema load of the same schema.
static int cmd_create_cost(graal_isolatethread_t* thread) {
    std::string id = baked_id(thread);
    printf("\n  library baked schema: %s\n\n", id.empty() ? "(none - generic build)" : id.c_str());
    if (!id.empty()) {
        double total = 0;
        const int N = 5;
        for (int i = 0; i < N; ++i) {
            const auto t0 = clk::now();
            exi_ctx c = make_ctx(thread, nullptr, 0);
            total += ms_since(t0);
            exi_destroy(thread, c);
        }
        printf("  exi_create(NULL)  [baked %s]   : %8.3f ms  (avg of %d)\n", id.c_str(), total / N, N);
    }
    if (!std::ifstream(g_schema).good()) {
        printf("  (schema file not found - skipping runtime-load comparison)\n\n");
        return 0;
    }
    const auto t0 = clk::now();
    exi_ctx c = make_ctx(thread, g_schema, 0);
    double load_ms = ms_since(t0);
    exi_destroy(thread, c);
    printf("  exi_create(\"%s\") : %8.1f ms  (runtime XSD load)\n\n", g_schema, load_ms);
    if (!id.empty()) printf("  baking makes context creation effectively free.\n\n");
    return 0;
}

// ----------------------------------------------------------------- options ----

// The W3C EXI options schema only allows <fragment/> as an empty element
// under <common> (see EXI_Options.xsd) - a plain substring match is enough
// to detect it without pulling in an XML parser here.
static bool wants_fragment(const std::string& options_doc) {
    return options_doc.find("<fragment") != std::string::npos;
}

// Fragment-mode encode input follows the library's envelope convention: a
// single well-formed XML document whose root is a transport wrapper that
// gets DISCARDED, with its children encoded as the fragment's roots. Strip
// any leading XML declaration and wrap the rest in a throwaway <batch> root.
static std::string wrap_batch(const std::string& xml) {
    size_t start = 0;
    if (xml.compare(0, 2, "<?") == 0) {
        size_t end = xml.find("?>");
        start = (end == std::string::npos) ? 0 : end + 2;
    }
    return "<batch>" + xml.substr(start) + "</batch>";
}

// Encodes each sample under both a plain context and one built from a W3C
// EXI options document, then demonstrates header-options interop: a stream
// encoded with EXI_OPT_OPTIONS_IN_HEADER carries its own coding options, so
// even a plain context (built with completely different options) decodes it.
static int run_options(graal_isolatethread_t* thread, const std::string& optionsPath,
                        const std::string& samplesPath) {
    std::vector<char> options_buf = read_file(optionsPath);
    if (options_buf.empty()) {
        fprintf(stderr, "Error: unreadable or empty options document '%s'\n", optionsPath.c_str());
        return 1;
    }
    const std::string options_doc(options_buf.begin(), options_buf.end());
    const bool fragment = wants_fragment(options_doc);
    const char* schema = effective_schema(thread);

    exi_ctx ctxA = nullptr, ctxB = nullptr;
    if (exi_create_with_options(thread, schema, options_buf.data(), options_buf.size(), 0, &ctxA) != EXI_OK)
        die(thread, "exi_create_with_options failed");
    ctxB = make_ctx(thread, schema, 0);

    std::vector<std::string> messages = collect_messages(samplesPath);
    if (messages.empty()) {
        fprintf(stderr, "Error: no .xml at '%s'\n", samplesPath.c_str());
        exi_destroy(thread, ctxA); exi_destroy(thread, ctxB);
        return 1;
    }

    printf("\n  options document: %s%s\n", optionsPath.c_str(),
           fragment ? "  (fragment: samples wrapped in a discarded <batch> envelope)" : "");
    printf("  %-38s %11s %11s %9s\n", "sample", "plain-bytes", "doc-bytes", "delta%");
    printf("  %s\n", std::string(74, '-').c_str());

    std::string first_input;   // the exact bytes handed to ctxA for the first sample
    int ok = 0;
    for (const auto& msg : messages) {
        std::vector<char> xml = read_file(msg);
        if (xml.empty()) continue;
        const std::string name = fs::path(msg).filename().string();

        char* plain_exi = nullptr; size_t plain_len = 0;
        if (exi_encode(thread, ctxB, xml.data(), xml.size(), &plain_exi, &plain_len) != EXI_OK)
            die(thread, ("plain encode failed for " + msg).c_str());

        const std::string doc_input = fragment ? wrap_batch(std::string(xml.begin(), xml.end()))
                                                : std::string(xml.begin(), xml.end());
        char* doc_exi = nullptr; size_t doc_len = 0;
        if (exi_encode(thread, ctxA, doc_input.data(), doc_input.size(), &doc_exi, &doc_len) != EXI_OK)
            die(thread, ("options-document encode failed for " + msg).c_str());
        if (first_input.empty()) first_input = doc_input;

        const double delta = plain_len ? (double(doc_len) - double(plain_len)) / double(plain_len) * 100.0 : 0.0;
        printf("  %-38s %11zu %11zu %8.1f%%\n", name.c_str(), plain_len, doc_len, delta);
        exi_free(thread, plain_exi);
        exi_free(thread, doc_exi);
        ++ok;
    }
    printf("\n");

    // Header-interop demo: recreate ctx A with EXI_OPT_OPTIONS_IN_HEADER so the
    // stream is self-describing, then decode it with the plain ctx B above -
    // header options take precedence over context options on decode/peek.
    exi_ctx ctxA_header = nullptr;
    if (exi_create_with_options(thread, schema, options_buf.data(), options_buf.size(),
                                 EXI_OPT_OPTIONS_IN_HEADER, &ctxA_header) != EXI_OK)
        die(thread, "exi_create_with_options (header) failed");

    char* foreign = nullptr; size_t foreign_len = 0;
    if (exi_encode(thread, ctxA_header, first_input.data(), first_input.size(), &foreign, &foreign_len) != EXI_OK)
        die(thread, "header-mode encode failed");

    char* decoded = nullptr; size_t decoded_len = 0;
    exi_status ds = exi_decode(thread, ctxB, foreign, foreign_len, &decoded, &decoded_len);
    char root[256]; size_t root_len = 0;
    exi_status ps = exi_peek_root(thread, ctxB, foreign, foreign_len, root, sizeof root, &root_len);
    if (ds != EXI_OK || ps != EXI_OK) die(thread, "header-interop decode/peek failed");
    printf("  header-interop: plain context decoded foreign stream OK (root %s)\n\n", root);

    exi_free(thread, decoded);
    exi_free(thread, foreign);
    exi_destroy(thread, ctxA_header);
    exi_destroy(thread, ctxA);
    exi_destroy(thread, ctxB);
    return ok > 0 ? 0 : 1;
}

// ------------------------------------------------------------------ uuid ----

// RFC 4122 Appendix C well-known namespace UUIDs (same tail, differ only in
// the first group).
static const char* uuid_namespace_alias(const std::string& alias) {
    if (alias == "dns")  return "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
    if (alias == "url")  return "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
    if (alias == "oid")  return "6ba7b812-9dad-11d1-80b4-00c04fd430c8";
    if (alias == "x500") return "6ba7b814-9dad-11d1-80b4-00c04fd430c8";
    return nullptr;
}

// Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (hyphens optional) into 16
// big-endian bytes. Returns false on malformed input.
static bool parse_uuid(const std::string& s, std::array<uint8_t, 16>& out) {
    std::string hex;
    for (char c : s) if (c != '-') hex.push_back(c);
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        unsigned v;
        if (std::sscanf(hex.c_str() + 2 * i, "%2x", &v) != 1) return false;
        out[i] = uint8_t(v);
    }
    return true;
}

static std::string format_uuid(const std::array<uint8_t, 16>& id) {
    char buf[37];
    std::snprintf(buf, sizeof buf,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
        id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    return buf;
}

// RFC 4122 §4.3 name-based UUID: hash(namespace_bytes_be || name_utf8), take
// the first 16 bytes, stamp the version nibble (byte 6 high nibble = 3 or 5)
// and the RFC variant (byte 8 top two bits = 10). Needs no exi_ctx/schema —
// this is pure hashing, independent of the EXI library.
static int run_uuid(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "Error: usage: %s uuid <v3|v5> <namespace> <name...>\n", argv[0]);
        return 2;
    }
    const std::string version = argv[2];
    bool v5;
    if (version == "v5") v5 = true;
    else if (version == "v3") v5 = false;
    else { fprintf(stderr, "Error: unknown uuid version '%s' (want v3 or v5)\n", version.c_str()); return 2; }

    const std::string ns_arg = argv[3];
    const char* ns_literal = uuid_namespace_alias(ns_arg);
    std::array<uint8_t, 16> ns{};
    if (!parse_uuid(ns_literal ? ns_literal : ns_arg, ns)) {
        fprintf(stderr, "Error: bad namespace '%s' (want dns|url|oid|x500 or a UUID)\n", ns_arg.c_str());
        return 2;
    }

    const bool single = (argc - 4) == 1;
    for (int i = 4; i < argc; ++i) {
        const std::string name = argv[i];
        std::vector<uint8_t> msg(ns.begin(), ns.end());
        msg.insert(msg.end(), name.begin(), name.end());

        std::array<uint8_t, 16> id{};
        if (v5) {
            auto digest = uuid_hash::sha1(msg.data(), msg.size());
            std::copy(digest.begin(), digest.begin() + 16, id.begin());
        } else {
            id = uuid_hash::md5(msg.data(), msg.size());
        }
        id[6] = uint8_t((id[6] & 0x0F) | (v5 ? 0x50 : 0x30));
        id[8] = uint8_t((id[8] & 0x3F) | 0x80);

        const std::string uuid_str = format_uuid(id);
        if (single) printf("%s\n", uuid_str.c_str());
        else printf("%s  %s\n", name.c_str(), uuid_str.c_str());
    }
    return 0;
}

// ----------------------------------------------------------------- main ----

static void usage(const char* prog) {
    printf("exi-demo - capability showcase for libexificient (v2 C API)\n\n"
           "Usage: %s <bench|peek|headers|errors|threads|create-cost> [options] [path]\n"
           "       %s options <options.xml> [samples-dir]  encode under a W3C EXI options\n"
           "                                                document; compare vs defaults\n"
           "       %s uuid <v3|v5> <namespace> <name...>\n\n"
           "Options:\n"
           "  -s, --schema <path>    XSD passed to exi_create (default: %s)\n"
           "  -n, --iterations <N>   bench/threads: iterations per message (default: 1)\n"
           "  -t, --threads <N>      threads: worker count (default: 4)\n"
           "  -h, --help             this help\n\n"
           "If the linked library reports a baked schema and -s is not passed,\n"
           "every subcommand uses the baked context (NULL schema) instead.\n\n"
           "uuid: RFC 4122 name-based ids, no schema/exi_ctx needed. namespace is\n"
           "an alias (dns|url|oid|x500) or an explicit UUID; each name arg prints\n"
           "one line ('<name>  <uuid>'), or just the UUID if there is only one.\n",
           prog, prog, prog, g_schema);
}

int main(int argc, char** argv) {
    std::string sub = argc > 1 ? argv[1] : "";
    if (sub.empty() || sub == "-h" || sub == "--help") { usage(argv[0]); return sub.empty() ? 2 : 0; }
    if (sub == "uuid") return run_uuid(argc, argv);

    long iterations = 1;
    int nthreads = 4;
    std::vector<std::string> positional;   // bench/peek/headers/threads: [path]; options: [options.xml] [samples-dir]
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "Error: %s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "-s" || a == "--schema") { g_schema = val(); g_schema_overridden = true; }
        else if (a == "-n" || a == "--iterations") iterations = std::max(1L, strtol(val(), nullptr, 10));
        else if (a == "-t" || a == "--threads") nthreads = std::max(1, int(strtol(val(), nullptr, 10)));
        else if (a[0] != '-') positional.push_back(a);
        else { fprintf(stderr, "Error: unknown option '%s'\n", a.c_str()); usage(argv[0]); return 2; }
    }
    std::string path = positional.empty() ? "samples/" : positional[0];

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
    else if (sub == "threads") rc = cmd_threads(isolate, thread, nthreads, iterations);
    else if (sub == "create-cost") rc = cmd_create_cost(thread);
    else if (sub == "options") {
        if (positional.empty()) {
            fprintf(stderr, "Error: usage: %s options <options.xml> [samples-dir]\n", argv[0]);
            rc = 2;
        } else {
            std::string samplesPath = positional.size() > 1 ? positional[1] : "samples/";
            rc = run_options(thread, positional[0], samplesPath);
        }
    }
    else { fprintf(stderr, "Error: unknown subcommand '%s'\n\n", sub.c_str()); usage(argv[0]); rc = 2; }

    graal_tear_down_isolate(thread);
    return rc;
}
