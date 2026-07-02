# exi-demo

A small C++ example that consumes the prebuilt
[exificient](https://github.com/M4lwar/exificient-native-image) Conan package to
schema-informed **EXI-compress** UCI XML messages and report the compression
ratio. Point it at a single `.xml` file or a directory of messages; it loads the
schema once via `exi_init`, then encodes and decodes each message (and can time
the codec).

No JDK or GraalVM is needed — the library is consumed as a prebuilt Conan package.

## Requirements

- Conan 2
- CMake >= 3.15
- A C++17 compiler: GCC/Clang on Linux, MSVC on Windows

## 1. Get the exificient package

The library is published as a prebuilt Conan package attached to the
[exificient-native-image releases](https://github.com/M4lwar/exificient-native-image/releases).
Download the archive for your platform:

- `conan-exificient-0.3.0-rc-linux-x86_64.tgz`
- `conan-exificient-0.3.0-rc-linux-arm64.tgz`
- `conan-exificient-0.3.0-rc-windows-x86_64.tgz`

## 2. Restore it into the local Conan cache

```sh
conan cache restore conan-exificient-0.3.0-rc-<os>-<arch>.tgz
```

## 3. Build

### Linux / macOS

Single-config generators put the Conan toolchain under `build/Release/generators/`:

```sh
conan install .
cmake -S . -B build/Release \
      -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/Release/generators/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

Binary: `build/Release/exi-demo`.

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

The schema is bundled under `schemas/` and `exi_init` loads it relative to the
working directory, so run from the repo root. The exificient shared library must
be on the loader path, which the generated `conanrun` script handles.

Linux:

```sh
source build/Release/generators/conanrun.sh
./build/Release/exi-demo -h
./build/Release/exi-demo samples/
```

Windows (cmd):

```bat
call build\generators\conanrun.bat
build\Release\exi-demo.exe -h
build\Release\exi-demo.exe samples\
```

## CLI

```
exi-demo [options] [path]

  path                   An .xml file, or a directory searched recursively for
                         *.xml files (default: EntityReport.xml)
  -s, --schema <path>    XSD schema passed to exi_init
                         (default: ./schemas/UCI_MessageDefinitions_v2_5_0.xsd)
  -n, --iterations <N>   Encode+decode each message N times and report timing
  -h, --help             Show this help and exit
```

## Example: compression survey

`samples/` contains one message of each of six UCI types:

```
  message                                        XML       EXI    saved
  ---------------------------------------------------------------------
  samples/entity.xml                            3466       259    92.5%
  samples/navigation-report.xml                 1168        81    93.1%
  samples/position-report-detailed.xml          2383       129    94.6%
  samples/position-report.xml                   1798       139    92.3%
  samples/task-command.xml                      1460       121    91.7%
  samples/task.xml                              1403        81    94.2%
  ---------------------------------------------------------------------
  6 message(s)                                 11678       810    93.1%
```

## Example: timing

`exi-demo -n 1000 EntityReport.xml` times the one-time `exi_init` (schema load +
grammar build) separately from the per-message `exi_encode`/`exi_decode`. The init
takes a few seconds (building EXI grammars from the ~8 MB UCI schema) while each
encode/decode is well under a millisecond — the cost is paid once at init.

## Notes

- Schemas are supplied at runtime and passed to `exi_init`; the `.xsd` (and any
  schema it imports) must exist on disk. This repo bundles the UCI schemas under
  `schemas/`.
- The exificient library — and the EXIficient codec it embeds — are MIT-licensed;
  see the [exificient-native-image](https://github.com/M4lwar/exificient-native-image)
  repo for details.
```
