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

| Artifact                      | Description              |
|-------------------------------|--------------------------|
| `build/howlang`               | Interpreter executable   |
| `build/libhowlang_frontend.a` | Parser static library    |
| `build/libhowlang_runtime.a`  | Runtime static library   |

## Running tests

From the repo root:

```bash
./c_interpreter/build/howlang samples/tests/test_all.how       # 54/54
./c_interpreter/build/howlang samples/tests/test_loops.how     # 41/41
./c_interpreter/build/howlang samples/tests/test_parallel.how  # 31/31
cd samples && ../c_interpreter/build/howlang tests/graph_test.how       # 32/32
cd samples && ../c_interpreter/build/howlang tests/lru_cache_test.how   # 34/34
cd samples && ../c_interpreter/build/howlang tests/brainfuck_test.how   # 32/32
```

## Parallel for-range

Howlang supports a parallel variant of the for-range loop using `^`:

```
# Sequential
var results = (i=0:n){ :: work(i) }()

# Parallel — iterations run concurrently, result list preserves index order
var results = (i=0:n)^{ :: work(i) }()
```

Semantics:
- Iterations run on a thread pool sized to the number of logical CPUs
- Each iteration gets its own local scope
- Writing to a variable declared outside the loop is a runtime error
- Reading outer variables is allowed
- `::` results are collected into a list in original index order; returns `none` if no `::` in body
- `break` is not allowed; `continue` works normally

`par(lst, fn)` is syntactic sugar for the common map pattern:

```
var doubled = par({1, 2, 3}, (x){ :: x * 2 })
# same as: (i=0:len(lst))^{ :: fn(lst(i)) }()
```
