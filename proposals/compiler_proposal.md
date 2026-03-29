# Howlang — Compiler Backend Proposal

## Starting point

The interpreter is in good shape. `howlang_frontend` (lexer + parser + AST) is
already a clean, standalone library. `howlang_runtime` has a working GC, value
system, and builtins. The modular split we just did (ad.c, import.c, parallel.c,
call.c) makes the runtime easier to navigate.

This proposal describes the **minimum changes** needed to add a bytecode compiler
and VM backend while keeping the tree-walking interpreter intact and all existing
tests passing.

---

## What's already reusable — zero changes needed

| Module | Reuse |
|---|---|
| `lexer.c`, `parser.c` | Fully reusable as-is |
| `ast.h` | Fully reusable; `is_ret`, `is_parallel`, `is_forrange` flags are useful, not a problem |
| `gc.c` — `Value`, GC mark/sweep | Fully reusable for the VM |
| `builtins.c`, `setup_globals()` | Fully reusable |
| `import.c` — `exec_import()` | Fully reusable |
| `call.c` — `eval_call_val()` | Reusable for mixed-mode (compiled calling interpreted and vice versa) |

The existing tree-walking interpreter is **not removed**. The bytecode path is
additive — you get both, and can migrate hot paths incrementally.

---

## The real blocker: name resolution

The ChatGPT review lists 10 issues. Nine of them are long-term polish. There is
one genuine blocker for a compiler backend:

**`eval()` resolves names at runtime by walking the `Env*` chain.** A compiler
cannot emit correct `LOAD`/`STORE` instructions without knowing statically:

- Is this name a local variable? → emit `OP_LOAD_LOCAL slot`
- Is it captured from an enclosing function? → emit `OP_LOAD_UPVAL slot`
- Is it a module-level global? → emit `OP_LOAD_GLOBAL name`

This requires a new **semantic analysis pass** over the AST — a single new file
before the compiler itself is possible. Everything else falls out from there.

---

## Three-phase plan

```
Phase 1 — Semantic analysis     sema.h / sema.c
Phase 2 — Bytecode design       bytecode.h
Phase 3 — Compiler + VM         compiler.c / vm.c
```

The existing tree-walker is untouched throughout. Each phase produces a working,
testable artifact before the next begins.

---

## Phase 1 — Semantic analysis pass

### New files

```
c_interpreter/include/sema.h
c_interpreter/src/sema.c
```

Add `sema` to `howlang_runtime` in CMakeLists.txt.

### What it does

One pass over the AST, bottom-up. For each function scope it:

1. Collects all `var` declarations and parameter names → local slot indices
2. Detects which names in inner functions reference outer function's locals →
   upvalues (captures)
3. Tags each `N_NAME` AST node with a resolved kind and slot index
4. Tags each function (`N_FUNC`) node with its local count and upvalue list

### Structs

```c
/* sema.h */

typedef enum { NAME_LOCAL, NAME_UPVAL, NAME_GLOBAL } NameKind;

/* Written into Node for N_NAME nodes after resolution */
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

`Node` in `ast.h` already has a `line` field and a large union. Add one field to
the `N_NAME` variant for the resolved info:

```c
/* In the N_NAME case of the Node union: */
struct { char *name; NameInfo resolved; } nname;
```

And add to the `N_FUNC` / `N_CLASS` node:
```c
int    nlocals;     /* filled by sema */
int    nupvals;     /* filled by sema */
char **upval_names; /* filled by sema */
```

### Error handling

`sema_resolve` accumulates errors in `ctx->errors` rather than calling `die()`.
This is the one place where we diverge from the interpreter's error style — the
compiler needs to report multiple errors per file.

---

## Phase 2 — Bytecode design

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

    /* Modules */
    OP_IMPORT,        /* A = modname const, B = alias const (0 = none) */

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

### Why stack-based over register-based

For howlang's expression-heavy style (everything is an expression, branches
return values) a stack machine is more natural to generate from the AST and
easier to get right first. A register-based IR can be added later as an
optimization pass on top.

---

## Phase 3 — Compiler + VM

### New files

```
c_interpreter/src/compiler.c
c_interpreter/src/vm.c
c_interpreter/include/vm.h
```

Add both to `howlang_runtime` in CMakeLists.txt.

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
| `N_NAME` (LOCAL) | `OP_LOAD_LOCAL slot` |
| `N_NAME` (UPVAL) | `OP_LOAD_UPVAL slot` |
| `N_NAME` (GLOBAL) | `OP_LOAD_GLOBAL const_idx(name)` |
| `N_NUM`, `N_STR`, `N_BOOL`, `N_NONE` | `OP_LOAD_CONST idx` |
| `N_BINOP "+"` (strings) | `OP_CONCAT` |
| `N_BINOP` (arithmetic) | compile l, compile r, `OP_ADD` etc. |
| `N_ASSIGN var` | compile rhs, `OP_STORE_LOCAL/UPVAL/GLOBAL` |
| `N_ASSIGN obj(k)=v` | compile obj, compile k, compile v, `OP_SET_INDEX` |
| `N_CALL` | compile callee, compile args..., `OP_CALL argc` |
| `N_FUNC` | emit `OP_MAKE_FUNC proto_idx nupvals` |
| `N_BRANCH` (the howlang `cond: body` list) | see below |
| `N_RETURN` (`::`) | compile expr, `OP_RETURN` |
| `N_FORRANGE` | compile start/stop, emit loop with `OP_JUMP_IF_FALSE` |
| `N_LOOP` (`(:)={}`) | emit loop with `OP_JUMP_IF_TRUE break_target` |
| `N_IMPORT` | `OP_IMPORT modname alias` |

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
compiled to bytecode can call an interpreted function and vice versa — `OP_CALL`
checks whether the callee is a `Proto`-backed function or a `HowFunc`, and
dispatches accordingly.

### Upvalue handling

Open upvalues point directly into the live stack frame. When a function call
returns and its frame pops, all upvalues that reference slots in that frame are
"closed" — the value is copied into `UpVal.closed` and `location` redirected.

This is the same model Lua uses and is the cleanest way to handle howlang's
closures (`make_adder`, `memoize`, etc.) without changing the value system.

---

## What does NOT change

- `howlang_frontend` — zero changes
- `gc.c`, `builtins.c` — zero changes
- `runtime.c`, `call.c`, `ad.c`, `parallel.c`, `import.c` — zero changes
- All existing test suites continue to pass
- The tree-walking interpreter remains the default until the VM is stable
- AD (`grad()`) continues to work through the interpreter path; tensor + VM
  integration is a separate future proposal

---

## Global state

The ChatGPT review flags global state as blocking. It's not blocking for a
bytecode VM that shares the interpreter's runtime — the VM is just another
consumer of `g_globals` and the GC. What does matter:

- `g_alloc_mutex` already protects GC insertions — the VM reuses this
- `g_gc_suspended` for parallel loops — the VM sets/clears this the same way
- The AD tape globals — untouched; `grad()` doesn't run through the VM initially

Global state becomes a problem if you want **multiple independent howlang
instances in the same process** (e.g., for an LSP or test harness). That is a
later refactor and doesn't block the compiler backend.

---

## Driver integration

Add a `-c` / `--compile` flag to `driver.c`:

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
    src/common.c src/lexer.c src/parser.c)

add_library(howlang_runtime STATIC
    src/runtime.c src/gc.c src/builtins.c
    src/call.c src/ad.c src/import.c src/parallel.c
    src/sema.c)             # ← Phase 1 addition

add_library(howlang_compiler STATIC  # ← Phase 3 addition
    src/compiler.c src/vm.c)

add_executable(howlang src/driver.c)
target_link_libraries(howlang
    howlang_compiler howlang_runtime howlang_frontend m Threads::Threads)
```

`howlang_compiler` depends on `howlang_runtime` (for `Value*`, GC, builtins) but
`howlang_runtime` does not depend on `howlang_compiler`. The tree-walker can be
built and shipped without the compiler.

---

## Milestone summary

| Phase | New files | Deliverable |
|---|---|---|
| 1 | `sema.h`, `sema.c` | Name resolution; every `N_NAME` knows its kind + slot |
| 2 | `bytecode.h` | Defined instruction set + `Proto` struct; no runtime yet |
| 3a | `compiler.c` | AST → bytecode; print disassembly with `-S` flag |
| 3b | `vm.c`, `vm.h` | Stack VM; runs compiled protos; passes all test suites |

After 3b: the MLP / tensor path can move to the VM for the advertised speedup,
the AD path stays on the interpreter, and both coexist cleanly.
