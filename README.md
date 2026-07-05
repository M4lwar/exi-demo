# exi-demo

A small C++ example that consumes the prebuilt
[exificient](https://github.com/M4lwar/exificient-native-image) Conan package to
schema-informed **EXI-compress** UCI XML messages. It's a capability showcase
for the library's v2 C ABI: eight subcommands, each exercising a different part
of the contract (compression + timing, type dispatch without a full decode,
the `$EXI` cookie header, structured error reporting, shared-context thread
safety, and encoding under a partner's W3C EXI options document).

No JDK or GraalVM is needed — the library is consumed as a prebuilt Conan
package.

> **v2 API note:** this demo targets `exificient >= 1.0.1` (the v2 C ABI —
> explicit `exi_ctx` contexts via `exi_create`/`exi_destroy`, not the old
> singleton `exi_init`). The earlier `exi_init`-based, single-command demo
> lives in this repo's git history prior to the `feat!: migrate to
> exificient v2 API` commit.

## Requirements

- Conan 2
- CMake >= 3.15
- A C++17 compiler: GCC/Clang on Linux, MSVC on Windows

The `exificient` package ships Linux (x86_64/arm64) and Windows (x86_64)
binaries only. On a macOS host, use the containerized `./bctl` helper below
instead of building natively.

## 1. Get the exificient package

The library is published as a prebuilt Conan package attached to the
[exificient-native-image releases](https://github.com/M4lwar/exificient-native-image/releases).
Download the archive for your platform:

- `conan-exificient-1.0.1-linux-x86_64.tgz`
- `conan-exificient-1.0.1-linux-arm64.tgz`
- `conan-exificient-1.0.1-windows-x86_64.tgz`

## 2. Restore it into the local Conan cache

```sh
conan cache restore conan-exificient-1.0.1-<os>-<arch>.tgz
```

## 3. Build

### Linux / macOS (native)

Single-config generators put the Conan toolchain under `build/Release/generators/`:

```sh
conan install .
cmake -S . -B build/Release \
      -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/Release/generators/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

Binary: `build/Release/exi-demo`.

### macOS host, containerized (`./bctl`)

The exificient package has no macOS binary, so on a macOS host build and run
inside a Linux container instead. `./bctl` wraps `podman run`, mounts the repo
at `/w`, and caches the Conan cache and apt/pip installs in named volumes
across invocations:

```sh
./bctl 'conan cache restore conan-exificient-1.0.1-linux-arm64.tgz && \
  conan install . && \
  cmake -S . -B build/Release \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/build/Release/generators/conan_toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build/Release && \
  ./build/Release/exi-demo bench samples/'
```

Any shell command can go inside the quotes — swap the last line for any
subcommand below, or chain several with `&&` as shown above.

### Windows (x86_64, MSVC)

MSVC is a multi-config generator, so the toolchain lands in `build\generators\`
(not `build\Release\generators\`) and the config is chosen at build time:

```bat
conan install .
:: CMAKE_TOOLCHAIN_FILE must be an absolute path — a relative one is resolved
:: against the build dir, not the current directory, and won't be found.
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%CD%\build\generators\conan_toolchain.cmake
cmake --build build --config Release
```

Binary: `build\Release\exi-demo.exe`.

## 4. Run

The schema is bundled under `schemas/` and `exi_create` loads it relative to
the working directory, so run from the repo root. `conan install` copies the
exificient shared library next to the executable, so it runs directly — no
environment setup.

Linux:

```sh
./build/Release/exi-demo -h
```

Windows (cmd):

```bat
build\Release\exi-demo.exe -h
```

## CLI

```
exi-demo - capability showcase for libexificient (v2 C API)

Usage: exi-demo <bench|peek|headers|errors|threads|create-cost> [options] [path]
       exi-demo options <options.xml> [samples-dir]  encode under a W3C EXI options
                                                document; compare vs defaults
       exi-demo uuid <v3|v5> <namespace> <name...>

Options:
  -s, --schema <path>    XSD passed to exi_create (default: ./schemas/UCI_MessageDefinitions_v2_5_0.xsd)
  -n, --iterations <N>   bench/threads: iterations per message (default: 1)
  -t, --threads <N>      threads: worker count (default: 4)
  -h, --help             this help

If the linked library reports a baked schema and -s is not passed,
every subcommand uses the baked context (NULL schema) instead.

uuid: RFC 4122 name-based ids, no schema/exi_ctx needed. namespace is
an alias (dns|url|oid|x500) or an explicit UUID; each name arg prints
one line ('<name>  <uuid>'), or just the UUID if there is only one.
```

`path` is an `.xml` file or a directory searched recursively for `*.xml`
files (default: `samples/`).

### Subcommands

| Subcommand | What it shows | Example |
|---|---|---|
| `bench` | Compression ratio per message, one-time `exi_create` cost, optional per-message encode/decode timing with `-n` | `exi-demo bench samples/` |
| `peek` | `exi_peek_root` identifies the message type without a full decode, and how its cost compares to a real decode | `exi-demo peek samples/` |
| `headers` | The `EXI_HEADER_COOKIE` flag: a plain-vs-cookie byte dump of the same message, and that a cookie-prefixed stream still decodes | `exi-demo headers samples/position-report.xml` |
| `errors` | Every `exi_status` failure mode (bad schema, bad flag, malformed XML/EXI, undersized buffer, stale/NULL context) paired with its `exi_last_error` message | `exi-demo errors` |
| `threads` | One shared `exi_ctx`, N worker threads each `graal_attach_thread`, concurrent `exi_encode` calls compared byte-for-byte against a reference | `exi-demo threads -t 4 -n 100` |
| `create-cost` | `exi_create(NULL)` against a baked context vs a full runtime XSD load of the same schema — why baking exists | `exi-demo create-cost` |
| `options` | `exi_create_with_options` against a W3C EXI options document: per-sample size vs a plain context, plus `EXI_OPT_OPTIONS_IN_HEADER` header-interop | `exi-demo options docs/options/byte-aligned.xml samples/` |
| `uuid` | RFC 4122 v3/v5 name-based ids — no schema or `exi_ctx` needed | `exi-demo uuid v5 dns www.example.com` |

### Samples

`samples/` has ten UCI 2.5 messages, all `xmllint`-validated against
`schemas/UCI_MessageDefinitions_v2_5_0.xsd`:

- Six small messages (`entity.xml`, `navigation-report.xml`,
  `position-report-detailed.xml`, `position-report.xml`,
  `task-command.xml`, `task.xml`) — one message type each, light on
  repeated strings.
- Four larger, string/UUID-dense messages (`flight-capability.xml`,
  `flight-capability-status.xml`, `nav-heavy.xml`, `posdet-heavy.xml`) that
  exercise a `FlightCapability`/`FlightCapabilityStatus`-style profile with
  many repeated enum tokens and cross-message UUID reuse (a status message
  referencing capability IDs declared in a companion capability-advertisement
  message) — useful for seeing how EXI compression behaves on
  string-heavy payloads rather than the numeric/kinematic fields the six
  smaller samples emphasize.

Every UUID in every sample is a deterministic UUIDv5 (RFC 4122, name-based,
not random v4): a fixed namespace UUID derived once via
`uuid5(NAMESPACE_DNS, "exi-demo.example.com")`, then each entity's ID is
`uuid5(namespace, "<role>/<name>")` for a self-documenting name string (e.g.
`system/uav-01`, `capability/flight/must-fly-primary`). This makes every ID
in `samples/` reproducible from its name rather than randomly generated, and
lets the same logical entity share one UUID across multiple sample files.

Output below is from an actual run against the released `exificient/1.0.1`
package (linux-arm64, containerized via `./bctl`).

#### `bench`

```
$ ./build/Release/exi-demo bench samples/

  Schema     : ./schemas/UCI_MessageDefinitions_v2_5_0.xsd
  exi_create : 8761.389 ms  (one-time: schema load + grammar build)

  message                                        XML       EXI    saved
  ---------------------------------------------------------------------
  samples/entity.xml                            3533       207    94.1%
  samples/flight-capability-status.xml          6588      1118    83.0%
  samples/flight-capability.xml                 6283       672    89.3%
  samples/nav-heavy.xml                         2153       340    84.2%
  samples/navigation-report.xml                 1170        83    92.9%
  samples/posdet-heavy.xml                      3377       397    88.2%
  samples/position-report-detailed.xml          2385       131    94.5%
  samples/position-report.xml                   1800       140    92.2%
  samples/task-command.xml                      1462       122    91.7%
  samples/task.xml                              1405        83    94.1%
  ---------------------------------------------------------------------
  10 message(s)                                30156      3293    89.1%
```

`exi_create` pays the schema-parse + grammar-build cost once (seconds, for
the ~8 MB UCI schema); every `exi_encode`/`exi_decode` afterward is well
under a millisecond. Reuse a context for the process lifetime rather than
recreating it per message.

The four string/UUID-dense samples save noticeably less than the six small
ones (83-89% vs. 92-94%): more distinct string/enum/UUID literals per
message means more string-table setup cost that a single-message encode
can't amortize — expected, and consistent with the fragment-vs-separate
findings that motivated adding these samples.

#### `peek`

```
$ ./build/Release/exi-demo peek samples/

  message                                  peeked type                       peek ms    decode ms
  ------------------------------------------------------------------------------------------------
  entity.xml                               Entity                              0.006        0.162
  flight-capability-status.xml             FlightCapabilityStatus              0.003        0.161
  flight-capability.xml                    FlightCapability                    0.002        0.144
  nav-heavy.xml                            NavigationReport                    0.004        0.123
  navigation-report.xml                    NavigationReport                    0.003        0.100
  posdet-heavy.xml                         PositionReportDetailed              0.003        0.119
  position-report-detailed.xml             PositionReportDetailed              0.002        0.085
  position-report.xml                      PositionReport                      0.002        0.078
  task-command.xml                         TaskCommand                         0.002        0.089
  task.xml                                 Task                                0.002        0.077

  peek reads only the stream head — cost is independent of message size.
```

`exi_peek_root` was 20x-60x faster than a full decode across this sample
set — useful for routing a message by type before deciding whether (and how)
to decode it. `flight-capability.xml`/`flight-capability-status.xml` peek as
`FlightCapability`/`FlightCapabilityStatus`; `nav-heavy.xml` and
`posdet-heavy.xml` are the same underlying `NavigationReport` /
`PositionReportDetailed` message types as their smaller counterparts, just
with denser string/UUID content.

#### `headers`

```
$ ./build/Release/exi-demo headers samples/position-report.xml

  message: samples/position-report.xml (1800 bytes XML)

  default:       80 6f c4 03 ac 7c 09 8e 53 a0 85 a5 01 51 52 c0   |.o...|..S....QR.|
  with cookie:   24 45 58 49 80 6f c4 03 ac 7c 09 8e 53 a0 85 a5   |$EXI.o...|..S...|

  cookie stream starts with "$EXI": yes;  size cost: 4 bytes (140 -> 144)
  cookie stream decodes via plain ctx: yes
```

`EXI_HEADER_COOKIE` prefixes the stream with the 4-byte ASCII cookie `$EXI`
for out-of-band format identification; a context created without the flag
can still decode a cookie-prefixed stream.

#### `errors`

```
$ ./build/Release/exi-demo errors

  create(missing schema xsd)             -> EXI_ERR_SCHEMA_LOAD        EXIException - XML Schema document (no/such/schema.xsd) not found.: NullPointerException
  create(unknown flag bit 5)             -> EXI_ERR_INVALID_ARG        IllegalArgumentException - unknown flag bits: 0x20
  encode("this is not xml")              -> EXI_ERR_MALFORMED_XML      SAXParseException - Content is not allowed in prolog.
  decode(garbage bytes)                  -> EXI_ERR_MALFORMED_EXI      TransformerException - org.xml.sax.SAXException: EXI No valid EXI document according distinguishing bits
  peek(name_cap=3) [needs 15]            -> EXI_ERR_BUFFER_TOO_SMALL   name buffer too small: need 15 bytes
  decode(NULL ctx)                       -> EXI_ERR_INVALID_CONTEXT    ctx is not a live context
  encode(destroyed ctx)                  -> EXI_ERR_INVALID_CONTEXT    ctx is not a live context

  every failure: a status code now, a message via exi_last_error(thread,...).
```

Every fallible call returns an `exi_status`; on failure, `exi_last_error`
retrieves a human-readable diagnostic for the calling thread's most recent
failure.

#### `threads`

```
$ ./build/Release/exi-demo threads -t 4 -n 100

  4 thread(s) x 100 iterations on ONE shared exi_ctx
  encodes:    400 ok, 0 failed, 0 byte-mismatches
  throughput: 7359 encodes/s (0.054 s total)
```

An `exi_ctx` is immutable after creation and safe to call concurrently from
any number of threads attached to the isolate (`graal_attach_thread`) — no
external locking needed.

#### `create-cost`

Output below is from a run against the **baked** `uci-2.5.0` package (see
[Baked library](#baked-library)), where `create-cost` is most interesting:

```
$ ./build/Release/exi-demo create-cost

  library baked schema: uci-2.5.0

  exi_create(NULL)  [baked uci-2.5.0]   :    0.012 ms  (avg of 5)
  exi_create("./schemas/UCI_MessageDefinitions_v2_5_0.xsd") :  10182.8 ms  (runtime XSD load)

  baking makes context creation effectively free.
```

Against a generic build (no baked schema), `create-cost` prints only the
runtime-load line — there's nothing baked to compare against.

#### `options`

```
$ ./build/Release/exi-demo options docs/options/byte-aligned.xml samples/

  options document: docs/options/byte-aligned.xml
  sample                                 plain-bytes   doc-bytes    delta%
  --------------------------------------------------------------------------
  entity.xml                                     207         445    115.0%
  navigation-report.xml                           83         162     95.2%
  position-report.xml                            140         255     82.1%
  task.xml                                        83         168    102.4%
  ...                                             (ten sample rows in a real run against samples/)

  header-interop: plain context decoded foreign stream OK (root Entity)
```

The `doc-bytes` column comes from a context built with
`exi_create_with_options` from the given W3C EXI options document (spec
§5.4 `<header>`); `plain-bytes` is the same message under a default
`exi_create` context, so the delta shows exactly what that document's
options cost or save. The header-interop line recreates the options
context with `EXI_OPT_OPTIONS_IN_HEADER`, encodes one message, and decodes
that stream with the plain context from the table above — proving the
stream is self-describing: an EXI header carrying options takes precedence
over whatever options the decoding context was created with, so a partner
using different settings can still read it.

This repo ships three example documents under `docs/options/`:
`byte-aligned.xml` (byte alignment), `partner-interop.xml` (byte alignment +
a capped block size + a capped value length + strict schema conformance),
and `compression-fragment.xml` (compression + fragment batching — samples
are wrapped in a `<batch>` envelope per the library's fragment convention,
detected by a `<fragment` substring match on the document). Any other
partner's W3C EXI options document works the same way; nothing here is
specific to these three.

#### `uuid` — RFC 4122 v3/v5 ids

Unlike the other subcommands, `uuid` needs no schema and makes no
`exi_ctx`/library call at all — it's a standalone RFC 4122 name-based UUID
generator (MD5/SHA-1 reference implementations vendored in
[`uuid_hash.hpp`](uuid_hash.hpp)), included here because it's what produced
every UUID in `samples/` (see [Samples](#samples) above):

```
$ ./build/Release/exi-demo uuid v5 dns www.example.com
2ed6657d-e927-568b-95e1-2665a8aea6a2

$ ./build/Release/exi-demo uuid v3 dns www.example.com
5df41881-3aed-3515-88a7-2f4a814cf09e

$ ./build/Release/exi-demo uuid v5 dns exi-demo.example.com
3dd65e4d-9f1b-58dc-b5c1-5aa11c55bdbc
```

The third line reproduces the samples' own namespace id — proof that every
UUID under `samples/` is regenerable with this tool rather than pasted in by
hand. Capability UUIDs like these are exactly what make UCI messages
string-table-heavy (a `FlightCapability`/`FlightCapabilityStatus` pair reuses
the same id string across messages), which is why fragment encoding plus
compression wins on that kind of content — see the string/UUID-dense samples
discussion under [`bench`](#bench) above for the measured effect.

## Baked library

`exificient` also ships a variant with a schema's grammars compiled directly
into the native image, selected with the `baked_schema` Conan option:

```sh
conan install . -o "exificient/*:baked_schema=uci-2.5.0"
```

This resolves to a separate package_id under the same `exificient/1.0.1`
recipe/version — restoring a baked `.tgz` into the Conan cache adds it
alongside any generic package already there; `conan list "exificient/1.0.1:*"`
shows both.

When the linked library reports a baked schema, every `exi-demo` subcommand
passes `NULL` as the schema to `exi_create` automatically — unless `-s` is
given, which always forces the explicit path and disables the baked default
for that run. `create-cost` above is what that buys: context creation drops
from seconds (parsing and building grammars from the `.xsd` at runtime) to
effectively free (the grammars are already compiled in).

This only applies to the Linux legs of CI, which restore
`conan-exificient-1.0.1+uci-2.5.0-linux-{x86_64,arm64}.tgz` from the
[exi-bake-template](https://github.com/M4lwar/exi-bake-template) releases.
The Windows leg stays on the generic `exificient/1.0.1` package — no baked
Windows artifact is published — so `create-cost` there only ever exercises
the runtime-load path.

## Notes

- Schemas are supplied at runtime and passed to `exi_create`; the `.xsd`
  (and any schema it imports) must exist on disk. This repo bundles the UCI
  schemas under `schemas/`.
- The exificient library — and the EXIficient codec it embeds — are
  MIT-licensed; see the
  [exificient-native-image](https://github.com/M4lwar/exificient-native-image)
  repo for details.
