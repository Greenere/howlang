# Compiler Subsystem

This folder holds the in-progress compiler backend for Howlang.

The current goal is not full language parity yet. The goal is to grow the
backend in small, testable slices while keeping the tree-walking interpreter as
the reference implementation.

## Files

- `CMakeLists.txt`
  Defines the `howlang_compiler` static library.
- `sema.c`
  Semantic analysis / name-resolution pass over the real frontend AST.
- `compiler.c`
  AST to bytecode compiler for the currently supported subset.
- `vm.c`
  Tiny stack VM for executing that bytecode subset.

## Implemented

### Semantic analysis

`sema.c` currently:

- resolves identifiers into `NAME_LOCAL`, `NAME_UPVAL`, or `NAME_GLOBAL`
- records closure metadata on AST nodes via `closure_info`
- validates duplicate named arguments
- validates direct named calls against inline function/class parameter lists
- powers `./build/howlang --check file.how`

The sema pass writes metadata into:

- `Node.resolved`
- `Node.closure_info`

### Bytecode / VM foundation

`compiler.c` and `vm.c` currently support a small but real compiled path:

- literals: numbers, strings, bools, `none`
- global identifier loads
- global `var` declarations
- global assignment with `=`
- unary `-` and `not`
- arithmetic and comparisons
- positional calls
- top-level unconditional expression statements
- top-level conditional branch statements of the form `cond: body`
- top-level function literals via `MAKE_FUNC_AST`

### Mixed-mode function support

Compiled top-level code can define plain function literals and call them.

Important nuance:

- the top-level script may be compiled
- function literals are currently materialized as ordinary interpreter-side
  `VT_FUNC` values
- those function bodies still execute through the interpreter path

So this works today:

```how
var add1 = (x){ :: x + 1 }
print(add1(4))
```

but it is still mixed-mode, not a fully compiled function body.

### Driver support

The CLI currently exposes:

- `--check`
  Parse + run semantic analysis only
- `--dis`
  Parse + sema + compile + disassemble
- `--compile`
  Parse + sema + compile + execute on the VM

## Current bytecode shape

The current instruction set is intentionally minimal:

- `LOAD_CONST`
- `LOAD_GLOBAL`
- `DEFINE_GLOBAL`
- `STORE_GLOBAL`
- `MAKE_FUNC_AST`
- `ADD`, `SUB`, `MUL`, `DIV`, `MOD`
- `NEG`
- `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`
- `NOT`
- `JUMP`
- `JUMP_IF_FALSE`
- `CALL`
- `POP`
- `RETURN`
- `RETURN_NONE`

This is enough to support straight-line programs plus simple top-level
conditional branches.

## Pending

These are the main missing pieces, roughly in order of usefulness:

### Next likely step

- local slots in compiled code
- compiled function bodies for ordinary `N_FUNC`
- plain function calls without falling back to AST-backed function literals

That would move the backend from "compiled top-level script with interpreter
functions" to "real compiled functions."

### Still missing

- closures / upvalues
- named-call compilation
- short-circuit `and` / `or`
- lists, maps, indexing, dot access
- classes and instances
- `for` loops and loop callables
- `break` / `next`
- imports
- `catch` / `throw`
- parallel loops
- tensor-specific compiler behavior
- AD-specific compiler behavior

## Known design choices

- The interpreter remains the semantic source of truth.
- The compiler backend is allowed to be mixed-mode for a while.
- We are favoring honest, narrow slices over pretending to have full support.
- `MAKE_FUNC_AST` is intentionally transitional; it gives us real progress now
  without forcing closure ABI work too early.

## Recommended workflow

From repo root:

```bash
cmake --build howlang_c/build
./howlang_c/build/howlang --check path/to/file.how
./howlang_c/build/howlang --dis path/to/file.how
./howlang_c/build/howlang --compile path/to/file.how
```

For regressions, always also run:

```bash
./howlang_c/build/howlang samples/tests/test_all.how
```

That keeps the compiler work honest against the existing interpreter behavior.
