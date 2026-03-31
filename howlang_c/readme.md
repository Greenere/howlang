# howlang C interpreter — build instructions

## Prerequisites

- A C compiler (macOS: `clang` via Xcode Command Line Tools; Linux: `gcc` or `clang`)
- CMake 3.16 or newer
- POSIX threads (`pthread`) — provided by the OS on macOS and Linux; no extra install needed

### macOS setup

```bash
xcode-select --install   # Xcode Command Line Tools (if needed)
brew install cmake        # CMake (if needed)
```

## Build

From this directory (`howlang_c/`):

```bash
cmake -S . -B build
cmake --build build
```

The executable is at `build/howlang`.

## Build types

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   && cmake --build build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Clean rebuild

```bash
rm -rf build
cmake -S . -B build
cmake --build build
```

## Project layout

```
howlang_c/
  CMakeLists.txt
  include/
    shared/
      common.h            # Shared utilities: buffers, memory, error reporting, REPL state
      ast.h               # AST node types and structures
    frontend/
      lexer_internal.h    # Token types shared between lexer and parser (internal)
    compiler/
      sema.h              # Semantic analysis / name-resolution pass API
      compiler.h          # AST -> bytecode compiler entry points
      bytecode.h          # Bytecode and proto definitions
      vm.h                # Stack VM API
    interpreter/
      runtime.h           # Public runtime API
      runtime_internal.h  # Private interpreter internals shared across translation units
  src/
    shared/
      common.c           # Implementations of shared utilities
    compiler/
      CMakeLists.txt     # Compiler-backend target definition
      sema.c            # Semantic analysis pass: scope tracking, identifier resolution, call validation
    cli/
      driver.c          # CLI entry point and REPL
    frontend/
      lexer.c           # Tokeniser
      parser.c          # Parser and AST construction
    interpreter/
      gc.c              # GC state, value/env/map/list constructors, mark-sweep collector
      runtime.c         # Evaluator and public bootstrap/run API
      ad.c              # Automatic differentiation support
      builtins.c        # Built-in functions and global environment setup
      call.c            # Function/class invocation and loop-call dispatch
      import.c          # Module loading and import search paths
      parallel.c        # Parallel loop support
      tensor.c          # Tensor operations and tensor helpers
  build/                # CMake output (gitignored)
```

## Build outputs

| Artifact                      | Description              |
|-------------------------------|--------------------------|
| `build/howlang`               | Interpreter executable   |
| `build/libhowlang_frontend.a` | Parser static library    |
| `build/src/compiler/libhowlang_compiler.a` | Compiler frontend/backend helpers |
| `build/libhowlang_runtime.a`  | Runtime static library   |

## Running tests

From the repo root:

```bash
./howlang_c/build/howlang samples/tests/test_all.how       # 54/54
./howlang_c/build/howlang --check samples/tests/test_all.how
./howlang_c/build/howlang samples/tests/test_loops.how     # 41/41
./howlang_c/build/howlang samples/tests/test_parallel.how  # 31/31
./howlang_c/build/howlang samples/tests/test_named_args.how # 11/11
./howlang_c/build/howlang samples/tests/test_map_builtin.how # 12/12
./howlang_c/build/howlang samples/tests/test_reduce_builtin.how # 8/8
cd samples && ../howlang_c/build/howlang tests/graph_test.how       # 32/32
cd samples && ../howlang_c/build/howlang tests/lru_cache_test.how   # 34/34
cd samples && ../howlang_c/build/howlang tests/brainfuck_test.how   # 32/32
```
