# howlang C interpreter — build instructions

## Prerequisites

- A C compiler (macOS: `clang` via Xcode Command Line Tools; Linux: `gcc` or `clang`)
- CMake 3.16 or newer

### macOS setup

```bash
xcode-select --install   # Xcode Command Line Tools (if needed)
brew install cmake        # CMake (if needed)
```

## Build

From this directory (`c_interpreter/`):

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
c_interpreter/
  CMakeLists.txt
  include/
    common.h            # Shared utilities: buffers, memory, error reporting, REPL state
    ast.h               # AST node types and structures
    frontend.h          # Public parser API
    runtime.h           # Public runtime API
    lexer_internal.h    # Token types shared between lexer and parser (internal)
    runtime_internal.h  # Types, globals, and declarations shared across runtime, gc, builtins (internal)
  src/
    common.c            # Implementations of common utilities
    lexer.c             # Tokeniser
    parser.c            # Parser and AST construction
    frontend.c          # AST list helpers (nl_push, sl_push, make_node, …)
    gc.c                # GC state, value/env/map/list constructors, mark-sweep collector
    builtins.c          # Built-in functions and global environment setup
    runtime.c           # Evaluator, module import, and public bootstrap/run API
    driver.c            # CLI entry point and REPL
  build/                # CMake output (gitignored)
```

## Build outputs

| Artifact                  | Description              |
|---------------------------|--------------------------|
| `build/howlang`           | Interpreter executable   |
| `build/libhowlang_frontend.a` | Parser static library |
| `build/libhowlang_runtime.a`  | Runtime static library |
