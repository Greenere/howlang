# Howlang — Compiler Backend Proposal

## Starting point

The interpreter is in good shape. `howlang_frontend` (lexer + parser + AST) is
already a clean, standalone library. `howlang_runtime` has a working GC, value
system, and builtins. The current shallow `src/` split is also a better
foundation than the original draft assumed:

- `src/frontend/` for lexer/parser/frontend helpers
- `src/core/` for the driver, GC, and tree-walking evaluator
- `src/runtime/` for builtins, calls, AD, tensors, imports, and parallel helpers

That organization makes the future compiler/VM layer feel like another explicit
subsystem rather than something that must be threaded through one monolithic
directory.

This proposal describes the **minimum changes** needed to add a bytecode compiler
and VM backend while keeping the tree-walking interpreter intact and all existing
tests passing.

---

## What's already reusable — with a few surgical additions

| Module | Reuse |
|---|---|
| `frontend/lexer.c`, `frontend/parser.c` | Fully reusable as-is |
| `ast.h` | Reusable as the frontend AST, but it will need compiler metadata fields |
| `core/gc.c` — `Value`, GC mark/sweep | Fully reusable for the VM |
| `runtime/builtins.c`, `setup_globals()` | Fully reusable |
| `runtime/import.c` — `exec_import()` | Fully reusable |
| `runtime/call.c` — `eval_call_val()` | Reusable as the mixed-mode interop boundary |

The existing tree-walking interpreter is **not removed**. The bytecode path is
additive — you get both, and can migrate hot paths incrementally.

Important correction: this is not literally "zero changes" to the runtime layer.
We will need a compiled-function representation in `Value`/`HowFunc` space, a VM
entry point, and driver plumbing. The point is that we can keep the interpreter
semantics and most of the runtime implementation intact.

---

## The real blockers: name resolution, function representation, and scope fidelity

The original version of this proposal understated the gap a little. Name
resolution is the first blocker, but there are a few more practical blockers we
should acknowledge up front:

1. **`eval()` resolves names at runtime by walking the `Env*` chain.**
   A compiler cannot emit correct `LOAD`/`STORE` instructions without knowing
   statically:

- Is this name a local variable? → emit `OP_LOAD_LOCAL slot`
- Is it captured from an enclosing function? → emit `OP_LOAD_UPVAL slot`
- Is it a module-level global? → emit `OP_LOAD_GLOBAL name`

2. **The runtime has no compiled callable representation today.**
   `Value` can hold `VT_FUNC` / `HowFunc`, but that object currently means
   "AST body + closure env". A VM needs a first-class compiled closure value:
   either a new `VT_COMPILED_FUNC` or a backend tag inside `HowFunc`.

3. **Howlang's scopes are not plain expression scopes.**
   `N_BRANCH`, `N_FORLOOP`, `N_CLASS`, named call args, `break` / `next`,
   and assignment-to-undeclared-variable rules all need to survive the lowering.

4. **The current language surface is already richer than the earliest draft assumed.**
   We now have named-argument calls, `map(...)` / `reduce(...)`, and custom
   gradients attached with `set_grad(f, rule)`. The compiler plan should target
   that real language surface, not an older pre-named-args / inline-grad model.

The right first step is still a semantic analysis pass, but the proposal should
explicitly treat the callable representation and scope rules as part of the MVP,
not as details that "fall out automatically."

---

## Revised phase plan

```
Phase 1 — Semantic analysis        sema.h / sema.c
Phase 2 — Bytecode + closure ABI   bytecode.h
Phase 3 — Core compiler + VM       compiler.c / vm.c
Phase 4 — Feature parity           classes/import/catch/parallel
```

The existing tree-walker is untouched throughout. Each phase produces a working,
testable artifact before the next begins.

---

## Phase 1 — Semantic analysis pass

### New files

```
c_interpreter/include/sema.h
c_interpreter/src/core/sema.c
```

Add `sema` to `howlang_runtime` in CMakeLists.txt.

### What it does

One pass over the AST, bottom-up. For each function scope it:

1. Collects all `var` declarations, loop variables, and parameter names
   into local slot indices
2. Detects which names in inner functions reference outer function locals
   and records upvalues
3. Resolves every `N_IDENT` load and assignment target to LOCAL / UPVAL / GLOBAL
4. Annotates each `N_FUNC`, `N_CLASS`, and `N_FORLOOP` with local-count / capture metadata
5. Validates compile-time-only invariants we currently discover late at runtime
   such as duplicate named args, unknown named args for statically known callees,
   and assignment to undeclared locals when resolvable statically
6. Records enough call-shape metadata that the compiler can preserve named-call
   behavior without rediscovering parameter layouts ad hoc later

### Structs

```c
/* sema.h */

typedef enum { NAME_LOCAL, NAME_UPVAL, NAME_GLOBAL } NameKind;

/* Written into identifier/assignment sites after resolution */
typedef struct {
    NameKind kind;
    int      slot;   /* local/upval slot index; -1 for globals */
} NameInfo;

/* Per-scope state during the walk */
typedef struct SemaScope {
    char             **locals;     /* local variable names in declaration order */
    int                nlocals;
    char             **upvals;     /* captured names, in capture order */
    int                nupvals;
    struct SemaScope  *parent;
    int                is_func;    /* 1 = function boundary, 0 = block */
} SemaScope;

typedef struct {
    SemaScope *scope;     /* current innermost scope */
    int        n_errors;
    char     **errors;    /* collected error messages */
} SemaCtx;

/* Public API */
SemaCtx *sema_new(void);
void     sema_resolve(Node *prog, SemaCtx *ctx);
void     sema_free(SemaCtx *ctx);
```

### Annotating AST nodes

The original draft said "add an `N_NAME` payload". That does not match the
current AST: the node kind is `N_IDENT`, and identifiers are currently stored in
`node->sval`. Because the existing union is already shared across many node
shapes, the least invasive plan is to add **generic compiler metadata fields**
outside the union instead of reshaping the node variants.

```c
/* In ast.h, adjacent to line/type rather than inside one union arm */
typedef struct {
    int      valid;   /* 1 once sema has resolved this site */
    NameKind kind;
    int      slot;    /* local/upval slot, -1 for globals */
} ResolvedName;

typedef struct {
    int    nlocals;
    int    nupvals;
    char **upval_names;
} ClosureInfo;
```

Then extend `Node` with something like:
```c
ResolvedName resolved;      /* used by N_IDENT and assignment targets */
ClosureInfo  closure_info;  /* used by N_FUNC / N_CLASS / N_FORLOOP */
```

This keeps the parser simple and avoids threading new mini-structs through every
existing union arm.

### Error handling

`sema_resolve` accumulates errors in `ctx->errors` rather than calling `die()`.
This is the one place where we diverge from the interpreter's error style — the
compiler needs to report multiple errors per file.

---

## Phase 2 — Bytecode design and closure ABI

### New file

```
c_interpreter/include/bytecode.h
```

### Instruction set

Stack-based, similar to Lua 5.x. All instructions are fixed-width 32-bit words:
`[opcode 8 bits | A 8 bits | B 8 bits | C 8 bits]` with a separate constant pool
per function prototype.

```c
/* bytecode.h */

typedef enum {
    /* Stack / locals */
    OP_LOAD_LOCAL,    /* A = slot → push locals[A] */
    OP_STORE_LOCAL,   /* A = slot ← pop into locals[A] */
    OP_LOAD_UPVAL,    /* A = slot → push upvals[A] */
    OP_STORE_UPVAL,   /* A = slot ← pop into upvals[A] */
    OP_LOAD_GLOBAL,   /* A = const_pool index (name string) */
    OP_STORE_GLOBAL,  /* A = const_pool index */
    OP_LOAD_CONST,    /* A = const_pool index → push Value */
    OP_LOAD_NONE,
    OP_LOAD_TRUE,
    OP_LOAD_FALSE,
    OP_POP,

    /* Arithmetic / comparison */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR, OP_NOT,

    /* Control flow */
    OP_JUMP,          /* A:B = signed 16-bit offset */
    OP_JUMP_IF_TRUE,  /* pop; jump if true */
    OP_JUMP_IF_FALSE, /* pop; jump if false (leaves value on stack) */

    /* Functions / calls */
    OP_MAKE_FUNC,     /* A = proto index, B = nupvals; pops B upval Values */
    OP_CALL,          /* A = argc; pops callee + argc args, pushes result */
    OP_CALL_NAMED,    /* A = argc, B = const index of arg-name vector */
    OP_RETURN,        /* pops top of stack, returns it */
    OP_RETURN_NONE,

    /* Collections */
    OP_MAKE_LIST,     /* A = n; pops n Values, pushes HowList */
    OP_MAKE_MAP,      /* A = n pairs; pops 2n Values (alternating key/val) */
    OP_INDEX,         /* pop obj, pop key → push obj(key) */
    OP_SET_INDEX,     /* pop obj, pop key, pop val → obj(key)=val */
    OP_GET_FIELD,     /* A = name const index; pop obj → push obj.field */
    OP_SET_FIELD,     /* A = name const index; pop obj, pop val */

    /* Augmented assignment helpers */
    OP_DUP,           /* duplicate top of stack */
    OP_DUP2,          /* duplicate top two stack values */

    /* Modules / errors */
    OP_IMPORT,        /* A = modname const, B = alias const (0 = none) */
    OP_THROW,
    OP_SETUP_CATCH,
    OP_END_CATCH,

    /* Misc */
    OP_CONCAT,        /* string + coercion, same as interpreter's + for strings */
} OpCode;

/* One compiled function */
typedef struct Proto {
    uint32_t  *code;       /* instruction array */
    int        code_len;
    int        code_cap;
    Value    **consts;     /* constant pool (nums, strings, nested protos) */
    int        nconsts;
    int        nlocals;    /* number of local variable slots */
    int        nupvals;    /* number of upvalues */
    int        nparams;    /* positional parameter count */
    char      *name;       /* debug: function name or "<anon>" */
    int       *lines;      /* parallel to code[], source line per instruction */
} Proto;
```

### Named arguments

This proposal needs one update for the current language: calls now carry
`arg_names` in the AST. We have two reasonable options:

1. **Canonicalize at compile time** when the callee is statically known
   (`N_IDENT` bound to a user function / class in the same module)
2. **Emit `OP_CALL_NAMED`** when arg names must be preserved at runtime
   (dynamic calls, builtins like `print(..., newline=false)`, and mixed-mode interop)

The pragmatic path is: implement both, but only use `OP_CALL_NAMED` when
canonicalization is impossible.

This is not an edge case anymore. Named arguments are now part of ordinary
modern Howlang code, so preserving them correctly is part of core language
support rather than a deferred compatibility detail.

### Why stack-based over register-based

For howlang's expression-heavy style (everything is an expression, branches
return values) a stack machine is more natural to generate from the AST and
easier to get right first. A register-based IR can be added later as an
optimization pass on top.

---

## Phase 3 — Compiler + VM

### New files

```
c_interpreter/src/core/compiler.c
c_interpreter/src/core/vm.c
c_interpreter/include/vm.h
```

Add these to a separate `howlang_compiler` target in CMakeLists.txt.

### Compiler

```c
/* compiler.c — AST → bytecode, runs after sema_resolve() */

typedef struct CompileCtx {
    Proto       *proto;       /* currently emitting into */
    Proto      **proto_stack; /* for nested function definitions */
    int          depth;
    SemaCtx     *sema;
} CompileCtx;

Proto *compile(Node *prog, SemaCtx *sema);
```

The compiler is a straightforward recursive descent over the annotated AST.
The key translation rules:

| AST node | Bytecode |
|---|---|
| `N_IDENT` (LOCAL) | `OP_LOAD_LOCAL slot` |
| `N_IDENT` (UPVAL) | `OP_LOAD_UPVAL slot` |
| `N_IDENT` (GLOBAL) | `OP_LOAD_GLOBAL const_idx(name)` |
| `N_NUM`, `N_STR`, `N_BOOL`, `N_NONE` | `OP_LOAD_CONST idx` |
| `N_BINOP "+"` (strings) | `OP_CONCAT` |
| `N_BINOP` (arithmetic) | compile l, compile r, `OP_ADD` etc. |
| `N_ASSIGN var` | compile rhs, `OP_STORE_LOCAL/UPVAL/GLOBAL` |
| `N_ASSIGN obj(k)=v` | compile obj, compile k, compile v, `OP_SET_INDEX` |
| `N_CALL` | compile callee, compile args..., `OP_CALL` / `OP_CALL_NAMED` |
| `N_FUNC` | emit `OP_MAKE_FUNC proto_idx nupvals` |
| `N_BRANCH` (the howlang `cond: body` list) | see below |
| `N_BRANCH` with `is_ret` | compile expr, `OP_RETURN` |
| `N_FORLOOP` | compile start/stop, emit loop with `OP_JUMP_IF_FALSE` |
| `N_FUNC` with `is_loop` | emit unbounded loop form |
| `N_IMPORT` | `OP_IMPORT modname alias` |
| `N_CATCH` | `OP_SETUP_CATCH`, body, handler, `OP_END_CATCH` |

### Compiling the branch model

The howlang branch model `cond: body, cond: body, ...` compiles to a chain of
conditional jumps. Each branch is independent (not `if/else`), so each condition
is tested in sequence:

```
for each branch:
    compile condition
    OP_JUMP_IF_FALSE  → skip_label
    compile body
    skip_label:
```

For `cond :: expr` (immediate-return branch):
```
    compile condition
    OP_JUMP_IF_FALSE  → skip_label
    compile expr
    OP_RETURN
    skip_label:
```

### Runtime callable representation

The VM cannot execute raw `Proto*` values directly because the current runtime
only knows how to call `VT_BUILTIN`, `VT_CLASS`, and `VT_FUNC`. The proposal
needs an explicit compiled closure object. Two viable designs:

1. Add `VT_COMPILED_FUNC` with:
   - `Proto *proto`
   - `Env *closure_globals` or module env
   - `UpVal **upvals`
2. Extend `HowFunc` with a backend tag:
   - `FUNC_AST`
   - `FUNC_PROTO`

I recommend **option 1** because it keeps the interpreter's `HowFunc` meaning
clean and minimizes accidental branching through AST-only fields in VM code.

One extra runtime constraint from the current codebase: user functions can now
carry custom gradient metadata via `set_grad(f, rule)`. For the first VM
milestone we should avoid coupling bytecode execution to AD internals. The
pragmatic plan is:

1. allow `grad(f)` / `set_grad(...)` to keep using the interpreter-side function model first
2. let compiled code call those builtins through mixed-mode dispatch
3. postpone "compiled closures carry native grad metadata" until after the VM is stable

### VM

```c
/* vm.h */

#define VM_STACK_MAX 2048
#define VM_CALL_DEPTH_MAX 512

typedef struct UpVal {
    Value  **location;   /* points into stack while open, into heap when closed */
    Value   *closed;     /* closed-over value after stack frame pops */
    int      is_closed;
} UpVal;

typedef struct CallFrame {
    Proto   *proto;
    uint32_t *ip;         /* instruction pointer */
    Value  **base;        /* base of this frame's local slots on the value stack */
    UpVal  **upvals;      /* upvalue array for this frame */
    int      nupvals;
} CallFrame;

typedef struct VM {
    Value      *stack[VM_STACK_MAX];
    Value     **sp;             /* stack pointer (next free slot) */
    CallFrame   frames[VM_CALL_DEPTH_MAX];
    int         nframes;
    Env        *globals;        /* shared with interpreter's g_globals */
} VM;

VM   *vm_new(void);
void  vm_free(VM *vm);
Value *vm_run(VM *vm, Proto *proto);
```

The VM **shares `g_globals` with the interpreter**. This means builtins,
imported modules, and user-defined globals are visible to both paths. A function
compiled to bytecode can call an interpreted function and vice versa — call
dispatch checks whether the callee is a compiled closure, `HowFunc`, builtin, or
class, and routes accordingly.

### Upvalue handling

Open upvalues point directly into the live stack frame. When a function call
returns and its frame pops, all upvalues that reference slots in that frame are
"closed" — the value is copied into `UpVal.closed` and `location` redirected.

This is the same model Lua uses and is the cleanest way to handle howlang's
closures (`make_adder`, `memoize`, etc.) without changing the value system.

---

## Builtin semantics cleanup already moving in the right direction

This is slightly adjacent to the compiler work, but it is worth writing down now
because the VM will be much easier to reason about if builtin dispatch follows a
small number of consistent rules.

The earlier draft treated this as mostly future cleanup. That is no longer
quite true. The language direction is already moving toward a cleaner split:

- `map(coll, fn)` is explicitly element-wise
- `reduce(coll, fn)` is now part of the collection surface
- `sum(x)` is a reduction for lists/tensors
- `abs(x)` is intended to stay scalar
- `max(...)` / `min(...)` are intended as scalar comparisons or reductions, not
  implicit element-wise broadcasts

That is a much better target for a compiler than the older model where unrelated
math builtins might secretly broadcast or reduce.

### Recommended rule

Use three semantic buckets and keep them distinct:

1. **Scalar transforms**
   `abs`, `sqrt`, `sin`, `cos`, `exp`, `log`, `pow`, `floor`, `ceil`

   These should mean "operate on one numeric value" (or dual number for AD).
   If the user wants element-wise collection behavior, they should write it
   explicitly via `map(coll, fn)`.

2. **Reductions**
   `sum`, `max`, `min`

   These may accept a collection and reduce it to one value, because that is a
   standard aggregate meaning rather than implicit broadcasting.

3. **Collection transforms / tensor primitives**
   `map`, `par`, `tensor`, `shape`, `T`, `outer`, `zeros`, `ones`, `eye`

   These are explicitly collection-oriented and can stay that way without
   ambiguity.

### Concrete recommendation for the current surface

- Keep `abs` scalar-only
- Keep `sum` as a list/tensor reduction
- Keep `max` / `min` as:
  - scalar comparison for multiple args
  - reduction for a single list or tensor
- Do **not** keep special implicit broadcast forms like `max(number, tensor)`; the
  element-wise version should be written as `map(t, (x){ :: max(c, x) })`
  or replaced by a dedicated domain helper such as `relu`

This gives a cleaner mental model:

- `map` is for element-wise transformation
- `sum` / `max` / `min` are reductions
- math builtins stay scalar unless they have a very standard aggregate meaning

### Why this helps the compiler too

This policy is not just aesthetic. It reduces the number of ad hoc builtin
shapes the compiler/VM may eventually want to special-case:

- scalar builtins remain ordinary calls
- reducers are easy to recognize semantically
- explicit element-wise operations stay explicit in the AST as `map(...)`

That keeps both interpreter and VM behavior easier to document, optimize, and
test.

### Compiler consequence

This no longer needs its own cleanup phase in the proposal. The compiler/VM
mainly benefits from the fact that:

- element-wise behavior stays explicit in the AST as `map(...)`
- reductions stay recognizable as `sum(...)`, `max(...)`, `min(...)`, `reduce(...)`
- scalar builtins remain ordinary calls

That is a much cleaner surface for future optimization and documentation.

---

## What does NOT change semantically

- `howlang_frontend` — zero semantic changes
- `core/gc.c`, `runtime/builtins.c` — largely unchanged
- The tree-walking interpreter remains the default until the VM is stable
- AD (`grad()`) continues to work through the interpreter path first

What *does* change:

- `ast.h` gains compiler metadata
- `runtime_internal.h` / `Value` gain a compiled callable representation
- `core/driver.c` gets compile / disassemble flags
- `runtime/call.c` becomes the mixed-mode boundary for compiled/interpreted calls

Also, because custom gradients now use `set_grad(f, rule)` instead of inline
function syntax, the compiler no longer needs to preserve a special parser/runtime
path for `func ... grad (...) { ... }`. That is one less frontend oddity to carry.

Also: "all existing test suites continue to pass" is too aggressive for the
first VM milestone. The realistic promise is:

- Phase 3 passes a **core subset**: expressions, variables, closures, loops,
  basic calls, maps/lists, and imports if included
- Phase 4 closes parity gaps for classes, catch/throw, parallel loops, and any
  remaining builtins / named-call corner cases

---

## Global state

The ChatGPT review flags global state as blocking. It's not blocking for a
bytecode VM that shares the interpreter's runtime — the VM is just another
consumer of `g_globals` and the GC. What does matter:

- `g_alloc_mutex` already protects GC insertions — the VM reuses this
- `g_gc_suspended` for parallel loops — the VM sets/clears this the same way
- The AD tape globals — untouched; `grad()` / `set_grad(...)` do not run through
  the VM initially

Global state becomes a problem if you want **multiple independent howlang
instances in the same process** (e.g., for an LSP or test harness). That is a
later refactor and doesn't block the compiler backend.

---

## Driver integration

Add a `-c` / `--compile` flag to `core/driver.c`:

```c
/* Compile + run via VM instead of tree-walking */
if (use_vm) {
    SemaCtx *sema = sema_new();
    sema_resolve(prog, sema);
    if (sema->n_errors) { /* print errors, exit */ }
    Proto *proto = compile(prog, sema);
    VM *vm = vm_new();
    vm_run(vm, proto);
    vm_free(vm);
    sema_free(sema);
} else {
    /* existing interpreter path */
    how_run_source(src, env);
}
```

---

## Phased CMake layout

```cmake
add_library(howlang_frontend STATIC
    src/frontend/common.c
    src/frontend/lexer.c
    src/frontend/parser.c)

add_library(howlang_runtime STATIC
    src/core/runtime.c
    src/core/gc.c
    src/runtime/tensor.c
    src/runtime/builtins.c
    src/runtime/call.c
    src/runtime/ad.c
    src/runtime/import.c
    src/runtime/parallel.c
    src/core/sema.c)             # ← Phase 1 addition

add_library(howlang_compiler STATIC  # ← Phase 3 addition
    src/core/compiler.c
    src/core/vm.c)

add_executable(howlang src/core/driver.c)
target_link_libraries(howlang
    howlang_compiler howlang_runtime howlang_frontend m Threads::Threads)
```

`howlang_compiler` depends on `howlang_runtime` (for `Value*`, GC, builtins) but
`howlang_runtime` does not depend on `howlang_compiler`. The tree-walker can be
built and shipped without the compiler.

---

## Recommended deliverables by phase

| Phase | New files | Deliverable |
|---|---|---|
| 1 | `sema.h`, `core/sema.c` | Name resolution on the real AST (`N_IDENT`, `N_FORLOOP`, named calls, closure metadata) |
| 2 | `bytecode.h` | Defined instruction set, closure ABI, and compiled callable representation |
| 3a | `core/compiler.c` | AST → bytecode for the core subset; `-S` disassembly |
| 3b | `core/vm.c`, `vm.h` | Stack VM for the core subset; mixed-mode calls work |
| 4 | compiler/vm follow-ups | Classes, catch/throw, parallel loops, full language parity |

After 3b: we have a credible end-to-end compiler backend. After Phase 4: we can
reasonably talk about flipping selected programs or hot functions to the VM.

## Suggested implementation order inside Phase 3

To keep momentum high, build the compiler backend in this order:

1. Literals, locals, globals, arithmetic, comparisons
2. `var`, assignment, blocks, returns
3. Function literals, closures, plain calls
4. Lists, maps, indexing, field access
5. `N_BRANCH`, `N_FORLOOP`, unbounded loop functions
6. Imports and module globals
7. Classes
8. `catch` / `throw`
9. Parallel loops

That sequence matches the current interpreter structure well and lets us land
useful milestones without pretending feature parity is immediate.

---

## Near-term non-goals

To keep this proposal credible, it helps to say clearly what the first compiler
backend does **not** need to solve:

- It does not need to compile AD internals or run tape logic inside the VM
- It does not need to optimize `map(...)` / `reduce(...)` callback-heavy code yet
- It does not need to subsume tensor-heavy samples into bytecode-native kernels
- It does not need to implement the wishlist from `tensor_ops_proposal.md`

The early success bar is narrower:

- compile ordinary expressions and closures correctly
- preserve named-call behavior
- interoperate cleanly with interpreter-side builtins and subsystems
- create a stable base for later optimization work
