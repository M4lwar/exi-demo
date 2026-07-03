# exi-demo v2 API Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate exi-demo to the exificient 1.0.0 v2 C API and restructure it into a capability showcase with subcommands (bench, peek, headers, errors, threads), per spec §3.7 of `exificient-native-image/docs/specs/2026-07-02-v2-c-interface-design.md`.

**Architecture:** One C++17 executable, `exi_demo.cpp`, with a tiny subcommand dispatcher. Shared plumbing (file IO, isolate/context lifecycle, error printing) at the top; one function per subcommand. Conan consumes the prebuilt `exificient/1.0.0` package; CMake unchanged except nothing — the target link is version-agnostic.

**Tech Stack:** C++17, Conan 2, CMake ≥3.15, podman (local builds run inside a Linux container — the package ships no macOS binaries), GitHub Actions CI.

## Global Constraints

- Repo: /Users/bzmadura/coding/exi-demo. Create and work on branch `v2-api`; push to `origin` (M4lwar/exi-demo). Do NOT push to `main`.
- Commit messages: conventional-commit style, NO AI/tool attribution anywhere (public repo).
- The v2 API contract is `exificient-native-image`'s `include/exificient.h` (ships inside the Conan package as `include/exificient.h`). Status codes `EXI_OK..EXI_ERR_INTERNAL`, flag `EXI_HEADER_COOKIE`, `EXI_API_VERSION`.
- **No macOS package exists.** Every local build/run happens in a Linux container:
  ```sh
  podman run --rm -v "$PWD":/w -w /w -v exi-demo-conan:/root/.conan2 \
      docker.io/library/python:3.12-bookworm bash -c '<commands>'
  ```
  with `apt-get update && apt-get install -y --no-install-recommends g++ cmake >/dev/null` and `pip -q install "conan>=2.0"` at the start of `<commands>` (the named volume caches the conan cache; apt/pip re-run per invocation — acceptable, ~40s).
- Package acquisition BEFORE the v1.0.0 release exists (Tasks 1–6): download the `conan-exificient-linux-<arch>.tgz` artifact from the latest green `build` workflow run on the library fork's `v2/c-interface` branch (`gh run download`), then `conan cache restore`. The tgz was packaged as version `0.0.0-ci` (non-tag CI builds use that version), so the conanfile pin during development is `exificient/0.0.0-ci`, switched to `exificient/1.0.0` in Task 7.
- On this arm64 Mac, use the `linux-arm64` artifact.
- Schema: the demo stays UCI-based (schemas/ are its own copies); `exi_create` takes `./schemas/UCI_MessageDefinitions_v2_5_0.xsd` — the v2 library is schema-neutral, nothing bundled.
- Samples' root elements (verify at runtime, used by peek assertions): position-report.xml → `PositionReport`; the other five samples print whatever they peek — do not hardcode expectations beyond PositionReport.

---

### Task 1: Branch, container build loop, package restore, red baseline

**Files:**
- Create: `bctl` (repo root, committed, executable — "build/run in container" helper)
- Create: `docs/plans/` (this file gets committed here too)

**Interfaces:**
- Produces: `./bctl '<shell commands>'` — runs commands in the pinned Linux container with conan cache volume; the `exificient/0.0.0-ci` package restored into that cache; proof the current `main` code does NOT compile against v2 (red baseline).

- [ ] **Step 1: Branch**

```sh
cd /Users/bzmadura/coding/exi-demo && git checkout -b v2-api
```

- [ ] **Step 2: Create `bctl`**

```sh
cat > bctl <<'EOF'
#!/bin/sh
# Build/run helper: executes the given shell command inside the Linux build
# container (the exificient package ships Linux/Windows binaries only, so
# macOS hosts build and run the demo in a container). The named volumes cache
# the Conan cache and apt/pip work across invocations.
exec podman run --rm -v "$PWD":/w -w /w -v exi-demo-conan:/root/.conan2 \
    docker.io/library/python:3.12-bookworm bash -c "
set -e
apt-get update -qq && apt-get install -y -qq --no-install-recommends g++ cmake >/dev/null
pip -q install 'conan>=2.0'
$*"
EOF
chmod +x bctl
```

- [ ] **Step 3: Download and restore the v2 package (0.0.0-ci) from the library fork's CI**

```sh
RUN_ID=$(gh run list --repo M4lwar/exificient-native-image --branch v2/c-interface \
         --workflow build --status success --limit 1 --json databaseId --jq '.[0].databaseId')
gh run download "$RUN_ID" --repo M4lwar/exificient-native-image \
   --name conan-exificient-linux-arm64 --dir /tmp/exi-pkg
ls /tmp/exi-pkg   # expect conan-exificient-arm64.tgz
cp /tmp/exi-pkg/conan-exificient-arm64.tgz .
./bctl 'conan profile detect --force >/dev/null 2>&1 || true; conan cache restore conan-exificient-arm64.tgz && conan list "exificient/*"'
```
Expected: `exificient/0.0.0-ci` listed in the cache. Do NOT commit the tgz (add `conan-exificient-*.tgz` to .gitignore in Step 5).

- [ ] **Step 4: Prove the red baseline — current code must FAIL against v2**

Temporarily set the requirement (not committed yet): in `conanfile.py`, change `self.requires("exificient/0.3.0-rc")` → `self.requires("exificient/0.0.0-ci")`. Then:
```sh
./bctl 'conan install . >/dev/null && cmake -S . -B build/Release -DCMAKE_TOOLCHAIN_FILE="$PWD/build/Release/generators/conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build/Release'
```
Expected: **compile FAILURE** — `exi_init` undeclared (v1 symbols are gone). This confirms the package is the real v2 artifact and the migration is genuinely needed.

- [ ] **Step 5: Commit the helper + gitignore (leave conanfile.py's changed pin in the working tree for Task 2's commit)**

```sh
printf 'conan-exificient-*.tgz\nbuild/\n' >> .gitignore
git add bctl .gitignore docs/plans/2026-07-03-v2-api-migration.md
git commit -m "build: containerized build loop for prebuilt-package development"
```

---

### Task 2: v2 skeleton + `bench` subcommand (feature parity with today's demo)

**Files:**
- Modify: `exi_demo.cpp` (full rewrite)
- Modify: `conanfile.py` (pin → `exificient/0.0.0-ci`, from Task 1 Step 4)

**Interfaces:**
- Produces: `exi-demo <subcommand> [options] [path]` CLI; subcommands `bench` (implemented here), `peek|headers|errors|threads` (stubs printing "not implemented" + exit 3, filled by Tasks 3–6). Shared helpers later tasks use verbatim: `read_file(path) -> std::vector<char>`, `collect_messages(path) -> std::vector<std::string>`, `die(thread, what)` (prints what + exi_last_error, exits 1), `make_ctx(thread, schema, flags) -> exi_ctx` (dies on failure), globals `g_schema` (default `./schemas/UCI_MessageDefinitions_v2_5_0.xsd`).

- [ ] **Step 1: Rewrite `exi_demo.cpp`** (complete file):

```cpp
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
    if (messages.empty()) { fprintf(stderr, "Error: no .xml at '%s'\n", path.c_str()); return 1; }

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

// ------------------------------------------------------- stubs (T3-T6) -----

static int cmd_peek(graal_isolatethread_t*, const std::string&)   { fprintf(stderr, "peek: not implemented\n");   return 3; }
static int cmd_headers(graal_isolatethread_t*, const std::string&){ fprintf(stderr, "headers: not implemented\n");return 3; }
static int cmd_errors(graal_isolatethread_t*)                     { fprintf(stderr, "errors: not implemented\n"); return 3; }
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
```
Note for the isolate: worker threads in Task 6 attach via `graal_attach_thread(isolate, ...)` — pass the isolate to `cmd_threads` then (change the stub signature in that task).

- [ ] **Step 2: Build and run in the container**

```sh
./bctl 'conan install . >/dev/null && cmake -S . -B build/Release -DCMAKE_TOOLCHAIN_FILE="$PWD/build/Release/generators/conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build/Release && source build/Release/generators/conanrun.sh && ./build/Release/exi-demo -h && ./build/Release/exi-demo bench samples/'
```
Expected: help text; bench table for the 6 samples with EXI well under 50% of XML; exit 0. Also verify stubs: `./build/Release/exi-demo peek` → "not implemented", exit 3.

- [ ] **Step 3: Commit**

```sh
git add exi_demo.cpp conanfile.py
git commit -m "feat!: migrate to exificient v2 API; subcommand CLI with bench"
```

---

### Task 3: `peek` subcommand — the dispatch-by-type showcase

**Files:**
- Modify: `exi_demo.cpp` (replace `cmd_peek` stub)

**Interfaces:**
- Consumes: helpers from Task 2. Produces: `exi-demo peek [path]`.

- [ ] **Step 1: Implement** (replaces the stub):

```cpp
// Simulates the bus-consumer pattern: identify each message's UCI type from
// the first bytes of its EXI form (no full decode), then "dispatch" it.
static int cmd_peek(graal_isolatethread_t* thread, const std::string& path) {
    exi_ctx ctx = make_ctx(thread, g_schema, 0);
    std::vector<std::string> messages = collect_messages(path);
    if (messages.empty()) { fprintf(stderr, "Error: no .xml at '%s'\n", path.c_str()); return 1; }

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
```

- [ ] **Step 2: Build + run**

```sh
./bctl 'cmake --build build/Release && source build/Release/generators/conanrun.sh && ./build/Release/exi-demo peek samples/'
```
Expected: 6 rows; `position-report.xml` row shows type `PositionReport`; peek ms visibly ≤ decode ms; exit 0.

- [ ] **Step 3: Commit**

```sh
git add exi_demo.cpp && git commit -m "feat: peek subcommand - message-type dispatch without decode"
```

---

### Task 4: `headers` subcommand — cookie on the wire

**Files:**
- Modify: `exi_demo.cpp` (replace `cmd_headers` stub)

- [ ] **Step 1: Implement**:

```cpp
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
```
Add `#include <cctype>` to the includes for `isprint`.

- [ ] **Step 2: Build + run** (same `./bctl` build command; run `exi-demo headers samples/position-report.xml`). Expected: two hexdump rows, second starting `24 45 58 49` ("$EXI"), size cost 4 bytes, decode-via-plain-ctx "yes".

- [ ] **Step 3: Commit** — `git add exi_demo.cpp && git commit -m "feat: headers subcommand - EXI cookie wire-format demo"`

---

### Task 5: `errors` subcommand — status codes + exi_last_error

**Files:**
- Modify: `exi_demo.cpp` (replace `cmd_errors` stub)

- [ ] **Step 1: Implement**:

```cpp
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
```

- [ ] **Step 2: Build + run** `exi-demo errors`. Expected rows: SCHEMA_LOAD, INVALID_ARG, MALFORMED_XML, MALFORMED_EXI, BUFFER_TOO_SMALL (needs 15), INVALID_CONTEXT ×2; non-empty message text on each failure; exit 0.

- [ ] **Step 3: Commit** — `git add exi_demo.cpp && git commit -m "feat: errors subcommand - status codes and per-thread diagnostics"`

---

### Task 6: `threads` subcommand — shared-context concurrency

**Files:**
- Modify: `exi_demo.cpp` (replace `cmd_threads` stub; plumb the isolate through)

- [ ] **Step 1: Implement.** Change the call site in `main` to `rc = cmd_threads(isolate, thread, nthreads, iterations);` and implement:

```cpp
// Demonstrates the documented thread-safety contract: one context, many
// attached threads, byte-identical results, near-linear encode throughput.
static int cmd_threads(graal_isolate_t* isolate, graal_isolatethread_t* thread,
                       int nthreads, long iterations) {
    if (iterations < 50) iterations = 50;
    std::vector<char> xml = read_file("samples/position-report.xml");
    if (xml.empty()) { fprintf(stderr, "Error: samples/position-report.xml unreadable\n"); return 1; }
    exi_ctx ctx = make_ctx(thread, g_schema, 0);

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
```
Update the forward declaration/stub signature accordingly.

- [ ] **Step 2: Build + run** `exi-demo threads -t 4 -n 100`. Expected: 400 ok, 0 failed, 0 mismatches; exit 0.

- [ ] **Step 3: Commit** — `git add exi_demo.cpp && git commit -m "feat: threads subcommand - shared-context concurrency demo"`

---

### Task 7: Release v1.0.0 of the library, repin, CI + README

**Files:**
- Modify (other repo): tag on `/Users/bzmadura/coding/exificient-native-image`
- Modify: `conanfile.py` (pin → `exificient/1.0.0`)
- Modify: `.github/workflows/ci.yml` (`EXIFICIENT_VERSION: "1.0.0"`; run subcommands)
- Modify: `README.md`

- [ ] **Step 1: Tag and release the library (USER-VISIBLE ACTION — the plan presenter must have user approval for tagging v1.0.0 before executing this step)**

```sh
cd /Users/bzmadura/coding/exificient-native-image
git tag v1.0.0 && git push fork v1.0.0
gh run list --repo M4lwar/exificient-native-image --limit 1   # tag-triggered run
# wait for completion; then verify assets:
gh release view v1.0.0 --repo M4lwar/exificient-native-image --json assets --jq '.assets[].name'
```
Expected assets: `conan-exificient-1.0.0-linux-{x86_64,arm64}.tgz`, `-windows-x86_64.tgz`, `libexificient-1.0.0-*.zip`. NOTE: the tag points at the v2/c-interface HEAD — confirm with the user whether to merge `v2/c-interface` → fork `master` first (tagging a non-master commit works but leaves master behind).

- [ ] **Step 2: Repin and reverify locally**

```sh
cd /Users/bzmadura/coding/exi-demo
# conanfile.py: self.requires("exificient/0.0.0-ci") -> self.requires("exificient/1.0.0")
curl -fsSL -o exi-1.0.0.tgz "https://github.com/M4lwar/exificient-native-image/releases/download/v1.0.0/conan-exificient-1.0.0-linux-arm64.tgz"
./bctl 'conan cache restore exi-1.0.0.tgz && rm -rf build && conan install . >/dev/null && cmake -S . -B build/Release -DCMAKE_TOOLCHAIN_FILE="$PWD/build/Release/generators/conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build/Release && source build/Release/generators/conanrun.sh && ./build/Release/exi-demo bench samples/ && ./build/Release/exi-demo peek samples/ && ./build/Release/exi-demo headers samples/position-report.xml && ./build/Release/exi-demo errors && ./build/Release/exi-demo threads'
```
Expected: all five subcommands exit 0 against the real released 1.0.0 package.

- [ ] **Step 3: CI workflow** — set `EXIFICIENT_VERSION: "1.0.0"`; in both jobs' Run steps replace the two run lines with:
```yaml
          ./build/Release/exi-demo -h
          ./build/Release/exi-demo bench samples/
          ./build/Release/exi-demo peek samples/
          ./build/Release/exi-demo headers samples/position-report.xml
          ./build/Release/exi-demo errors
          ./build/Release/exi-demo threads -t 4 -n 100
```
(and the cmd.exe equivalents with `|| exit /b 1` on Windows).

- [ ] **Step 4: README.md** — update: subcommand usage table (one line + example output snippet per subcommand), v2 API note ("requires exificient ≥ 1.0.0; v1-based demos live in git history"), keep the Windows toolchain-path gotcha section, build instructions gain the `./bctl` container path for macOS hosts.

- [ ] **Step 5: Commit, push, verify demo CI**

```sh
git add conanfile.py .github/workflows/ci.yml README.md
git commit -m "feat!: exificient 1.0.0; exercise all v2 subcommands in CI"
git push -u origin v2-api
gh run list --repo M4lwar/exi-demo --branch v2-api --limit 1   # then gh run watch <id> --exit-status
```
Expected: linux x86_64 + arm64 + windows all green. Merging `v2-api` → `main` is the user's call.

---

## Out of scope

- `create-cost` subcommand (needs Phase B baked builds).
- Conan remote hosting; macOS package.

## Execution notes

- Tasks 2–6 are fast container iterations (~1–2 min each after Task 1 warms the caches).
- Task 7 Step 1 is the only cross-repo, user-approval-gated action (public release tag).
