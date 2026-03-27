# howlang Refactor Build Instructions

This project has been split into three main parts:

- `frontend/` — lexer, AST helpers, parser
- `runtime/` — values, environment, GC, builtins, evaluator
- `driver/` — CLI entrypoint and REPL

It uses **CMake** as the build system.

## Prerequisites

You need:

- a C compiler
  - macOS: `clang` is usually available through Xcode Command Line Tools
  - Linux: `gcc` or `clang`
- **CMake** 3.16 or newer
- **Make** or another generator supported by CMake

### macOS setup

If needed, install Xcode Command Line Tools:

```bash
xcode-select --install
```

Install CMake with Homebrew if you do not already have it:

```bash
brew install cmake
```

## Build

From the project root:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This should produce the executable:

```bash
./howlang
```

## Run

To start the REPL:

```bash
./howlang
```

To run a source file:

```bash
./howlang path/to/program.how
```

## Clean rebuild

If you want to rebuild from scratch:

```bash
rm -rf build
mkdir build
cd build
cmake ..
cmake --build .
```

## Build type

For a debug build:

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

For a release build:

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Project outputs

The CMake setup is intended to build:

- `howlang_frontend` — frontend library
- `howlang_runtime` — runtime library
- `howlang` — interpreter executable

Depending on your platform and generator, the libraries may appear as static libraries such as:

- `libhowlang_frontend.a`
- `libhowlang_runtime.a`

## Suggested next step

A good next refactor is to move remaining interpreter-wide runtime state into an explicit runtime context struct, so the frontend can be reused more cleanly by a future compiler.
