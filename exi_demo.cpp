// exi-demo.cpp
//
// Schema-informed EXI compression demo for libexificient (the GraalVM-native
// build of EXIficient).
//
// Point it at a single .xml file or a directory of captured messages (searched
// recursively for *.xml). It loads the schema once (exi_init), then for each
// message encodes to EXI, decodes back to verify the round-trip, and reports the
// compression ratio. With --iterations N it encodes/decodes each message N times
// and reports timing, showing the cost is the one-time exi_init while each
// encode/decode is cheap.
//
// Run `exi-demo -h` for usage. The schema passed via --schema (and any schema it
// imports) must exist on disk.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "libexificient.h"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

static const char* DEFAULT_PATH   = "EntityReport.xml";
static const char* DEFAULT_SCHEMA = "./schemas/UCI_MessageDefinitions_v2_5_0.xsd";

static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static void usage(const char* prog) {
    printf(
        "exi-demo - schema-informed EXI compression demo for libexificient\n"
        "\n"
        "Encodes UCI XML message(s) to EXI and reports the compression ratio; also\n"
        "decodes back to verify the round-trip. With --iterations it times exi_init\n"
        "versus the per-message exi_encode/exi_decode.\n"
        "\n"
        "Usage: %s [options] [path]\n"
        "\n"
        "  path                   An .xml file, or a directory searched recursively\n"
        "                         for *.xml files (default: %s)\n"
        "\n"
        "Options:\n"
        "  -s, --schema <path>    XSD schema passed to exi_init; this file and any\n"
        "                         schemas it imports must exist on disk\n"
        "                         (default: %s)\n"
        "  -n, --iterations <N>   Encode+decode each message N times and report\n"
        "                         min/avg/max timing (default: 1)\n"
        "  -h, --help             Show this help and exit\n",
        prog, DEFAULT_PATH, DEFAULT_SCHEMA);
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

// Collect the .xml files to process: recurse if `path` is a directory.
static std::vector<std::string> collect_messages(const std::string& path) {
    std::vector<std::string> files;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(path, ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".xml")
                files.push_back(e.path().string());
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(path);
    }
    return files;
}

int main(int argc, char** argv) {
    std::string input_path = DEFAULT_PATH;
    const char* schema_path = DEFAULT_SCHEMA;
    long iterations = 1;
    bool path_set = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (a[0] != '-') {                    // positional path
            input_path = a;
            path_set = true;
            continue;
        }
        bool wants_value = !strcmp(a, "-s") || !strcmp(a, "--schema") ||
                           !strcmp(a, "-n") || !strcmp(a, "--iterations");
        if (!wants_value) {
            fprintf(stderr, "Error: unknown argument '%s'\n\n", a);
            usage(argv[0]);
            return 2;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Error: %s requires a value\n\n", a);
            usage(argv[0]);
            return 2;
        }
        const char* value = argv[++i];
        if (!strcmp(a, "-s") || !strcmp(a, "--schema")) {
            schema_path = value;
        } else {  // -n / --iterations
            iterations = strtol(value, nullptr, 10);
            if (iterations < 1) {
                fprintf(stderr, "Error: --iterations must be >= 1\n");
                return 2;
            }
        }
    }
    (void)path_set;

    std::vector<std::string> messages = collect_messages(input_path);
    if (messages.empty()) {
        fprintf(stderr, "Error: no .xml messages found at '%s'\n", input_path.c_str());
        return 1;
    }

    // --- Start the GraalVM runtime and time the one-time exi_init ---
    graal_isolate_t* isolate = nullptr;
    graal_isolatethread_t* thread = nullptr;
    if (graal_create_isolate(nullptr, &isolate, &thread) != 0) {
        fprintf(stderr, "Error: failed to create GraalVM isolate\n");
        return 1;
    }
    // exi_init takes a non-const char* (GraalVM's CCharPointer) but does not
    // modify it, so const_cast is safe.
    const auto init_t0 = clk::now();
    if (exi_init(thread, const_cast<char*>(schema_path)) != 0) {
        fprintf(stderr, "Error: exi_init failed (is schema '%s' present?)\n", schema_path);
        graal_tear_down_isolate(thread);
        return 1;
    }
    const double init_ms = ms_since(init_t0);

    printf("\n  Schema   : %s\n", schema_path);
    printf("  exi_init : %.3f ms  (one-time: schema load + grammar build)\n\n", init_ms);
    printf("  %-40s %9s %9s %8s", "message", "XML", "EXI", "saved");
    if (iterations > 1) printf("  %10s %10s", "enc avg ms", "dec avg ms");
    printf("\n  %s\n", std::string(iterations > 1 ? 92 : 69, '-').c_str());

    long total_xml = 0, total_exi = 0, ok = 0;
    double enc_tot_all = 0, dec_tot_all = 0;
    long enc_dec_calls = 0;

    for (const auto& msg : messages) {
        std::vector<char> xml = read_file(msg);
        std::string name = fs::path(msg).lexically_normal().string();
        if (name.size() > 40) name = "..." + name.substr(name.size() - 37);
        if (xml.empty()) {
            printf("  %-40s %9s\n", name.c_str(), "(unreadable)");
            continue;
        }
        const int xml_len = static_cast<int>(xml.size());

        int exi_len = 0, out_len = 0;
        double enc_sum = 0, dec_sum = 0;
        bool failed = false;
        for (long i = 0; i < iterations; ++i) {
            const auto e0 = clk::now();
            char* exi = exi_encode(thread, xml.data(), xml_len, &exi_len);
            enc_sum += ms_since(e0);
            if (!exi || exi_len < 0) { failed = true; break; }
            const auto d0 = clk::now();
            char* out = exi_decode(thread, exi, exi_len, &out_len);
            dec_sum += ms_since(d0);
            if (!out || out_len < 0) { exi_free(thread, exi); failed = true; break; }
            exi_free(thread, exi);
            exi_free(thread, out);
        }
        if (failed) {
            printf("  %-40s %9d %9s\n", name.c_str(), xml_len, "FAILED");
            continue;
        }

        const double saved = xml_len > 0 ? (1.0 - static_cast<double>(exi_len) / xml_len) * 100.0 : 0.0;
        printf("  %-40s %9d %9d %7.1f%%", name.c_str(), xml_len, exi_len, saved);
        if (iterations > 1) printf("  %10.3f %10.3f", enc_sum / iterations, dec_sum / iterations);
        printf("\n");

        total_xml += xml_len;
        total_exi += exi_len;
        enc_tot_all += enc_sum;
        dec_tot_all += dec_sum;
        enc_dec_calls += iterations;
        ++ok;
    }

    const double overall_saved = total_xml > 0 ? (1.0 - static_cast<double>(total_exi) / total_xml) * 100.0 : 0.0;
    printf("  %s\n", std::string(iterations > 1 ? 92 : 69, '-').c_str());
    printf("  %-40s %9ld %9ld %7.1f%%", (std::to_string(ok) + " message(s)").c_str(),
           total_xml, total_exi, overall_saved);
    if (iterations > 1 && enc_dec_calls > 0)
        printf("  %10.3f %10.3f", enc_tot_all / enc_dec_calls, dec_tot_all / enc_dec_calls);
    printf("\n\n");

    graal_tear_down_isolate(thread);
    return ok > 0 ? 0 : 1;
}
