# Howlang — Parallel For-Range Proposal - Implemented ✅

## Context

Howlang is a small language where everything is a function call. The primary
iteration primitive is the for-range loop:

```
(i=0:n){ body }()
```

This proposal adds a parallel variant using `^` as a signal that iterations
are independent.

Build the interpreter with:
```
cc -O2 -o howlang howlang.c -lm
```

All five test suites must pass after implementation:
```
./howlang samples/test_all.how          # 54/54
cd samples && ../howlang graph_test.how       # 32/32
cd samples && ../howlang lru_cache_test.how   # 34/34
cd samples && ../howlang brainfuck_test.how   # 32/32
./howlang test_loops.how                # 41/41
```

---

## The Proposal

### Syntax

```
(i=0:n)^{ body }()
```

One symbol change from the sequential form. `^` appears between `)` and `{`,
after the range declaration and before the body.

### Semantics

- All `n` iterations run simultaneously on a thread pool
- Each iteration gets its own local scope
- The body **cannot write to any variable declared outside the loop** — enforced
  at runtime, raises an error if violated
- The body CAN read outer scope freely
- `:: value` inside the body contributes that iteration's result to the output
- The loop call `(i=0:n)^{ :: expr }()` returns a **list of results in original
  index order** (not arrival order)
- If no `:: value` in body, the loop returns `none` (same as sequential)
- `continue` works as normal (skip rest of this iteration's body)
- `break` is **not available** inside `^{ }` — there is no shared iteration
  state to break from

### The one enforced invariant

```
var total = 0
(i=0:n)^{ total += items(i) }()
# RuntimeError: cannot write to outer variable 'total' in a parallel loop
#   --> file.how:2
#    2 | (i=0:n)^{ total += items(i) }()
```

The body can only write to variables declared inside the body itself.
Writing to anything in the enclosing scope is a runtime error.

### Examples

```
# Parallel map — square each element
var nums = {1, 2, 3, 4, 5, 6, 7, 8}
var squares = (i=0:len(nums))^{ :: nums(i) * nums(i) }()
print(squares)   # [1, 4, 9, 16, 25, 36, 49, 64]

# Parallel compute — expensive independent work
var results = (i=0:len(tasks))^{
    var task = tasks(i),
    :: process(task)
}()

# Reading outer scope is fine — just no writing
var multiplier = 10
var scaled = (i=0:len(nums))^{ :: nums(i) * multiplier }()

# No return value — parallel side effects (careful: IO ordering is undefined)
(i=0:len(files))^{
    var content = read(files(i)),
    write(outputs(i), transform(content))
}()
```

### `par(lst, fn)` builtin

Sugar for the most common case — apply a function to each element in parallel:

```
var results = par(items, expensive_fn)
# equivalent to: (i=0:len(items))^{ :: expensive_fn(items(i)) }()
```

---

## Implementation

### Step 1: Lexer — add `TT_CARET` token

In the `TT` enum, add `TT_CARET` alongside the other punctuation tokens.

In the `lex()` function `switch(c)` block, add:
```c
case '^': t.type = TT_CARET; break;
```

### Step 2: AST — add `LOOP_PARALLEL` to `LoopKind`

```c
typedef enum { LOOP_NONE, LOOP_UNBOUNDED, LOOP_FORRANGE, LOOP_PARALLEL } LoopKind;
```

`LOOP_PARALLEL` shares the same AST node structure as `LOOP_FORRANGE` — it has
`iter_var`, `loop_start`, `loop_stop`, and `branches`. No new fields needed.

### Step 3: Parser — detect `^` after `)` in for-range

In `parse_atom`, in the for-range detection block, after consuming
`IDENT EQ start COLON stop RPAREN`, check for `TT_CARET` before `{`:

```c
/* after: p_expect(p, TT_RPAREN, "expected ')'"); */
LoopKind lk = LOOP_FORRANGE;
if (p_match(p, TT_CARET)) {
    lk = LOOP_PARALLEL;
}
Node *n = make_node(N_FUNC, line);
n->func.loop_kind  = lk;          /* was always LOOP_FORRANGE before */
n->func.iter_var   = ivar;
n->func.loop_start = start;
n->func.loop_stop  = stop;
parse_func_body(p, &n->func.branches);
return n;
```

The existing `force_call` check in `parse_call` already handles for-range loops:
```c
int force_call = (n->type == N_FUNC &&
                  n->func.loop_kind == LOOP_FORRANGE &&
                  p_check(p, TT_LPAREN));
```
Update this to also force-call `LOOP_PARALLEL`:
```c
int force_call = (n->type == N_FUNC &&
                  (n->func.loop_kind == LOOP_FORRANGE ||
                   n->func.loop_kind == LOOP_PARALLEL) &&
                  p_check(p, TT_LPAREN));
```

### Step 4: Evaluator — `run_parallel_loop()`

In `eval_call_val`, the existing dispatch for `LOOP_FORRANGE` looks like:
```c
if (fn->loop_kind == LOOP_FORRANGE) { ... }
```

Add a new branch for `LOOP_PARALLEL` before or after it:
```c
if (fn->loop_kind == LOOP_PARALLEL) {
    return run_parallel_loop(fn, sig);
}
```

The `run_parallel_loop()` function:

```c
static Value *run_parallel_loop(HowFunc *fn, Signal *sig) {
    /* Evaluate start and stop in the closure environment */
    int start_v = 0;
    if (fn->loop_start) {
        Value *sv = eval(fn->loop_start, fn->closure, sig);
        if (sig->type != SIG_NONE) return sv;
        start_v = (int)sv->nval;
        val_decref(sv);
    }
    Value *stop_val = eval(fn->loop_stop, fn->closure, sig);
    if (sig->type != SIG_NONE) return stop_val;
    int stop_v = (int)stop_val->nval;
    val_decref(stop_val);

    int n = stop_v - start_v;
    if (n <= 0) return val_none();

    /* Pre-allocate results list — each thread writes to its own index slot */
    HowList *out = list_new();
    for (int i = 0; i < n; i++) list_push(out, val_none());

    /* Per-thread data */
    typedef struct {
        HowFunc  *fn;
        Env      *closure;    /* read-only: the outer closure */
        int       index;      /* which iteration this thread runs */
        int       iter_val;   /* the actual value of the loop variable */
        Value   **out_items;  /* pointer into pre-allocated results list */
        char     *error;      /* non-NULL if an error occurred */
    } ThreadArg;

    pthread_t   *threads = xmalloc(n * sizeof(pthread_t));
    ThreadArg   *args    = xmalloc(n * sizeof(ThreadArg));

    for (int i = 0; i < n; i++) {
        args[i].fn        = fn;
        args[i].closure   = fn->closure;
        args[i].index     = i;
        args[i].iter_val  = start_v + i;
        args[i].out_items = out->items;
        args[i].error     = NULL;
    }

    /* Thread worker */
    /* (define as a static function outside run_parallel_loop, see below) */
    for (int i = 0; i < n; i++)
        pthread_create(&threads[i], NULL, parallel_loop_worker, &args[i]);
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);

    /* Check for errors from any thread */
    char *first_error = NULL;
    for (int i = 0; i < n; i++) {
        if (args[i].error && !first_error)
            first_error = args[i].error;
    }

    free(threads);
    free(args);

    if (first_error) {
        /* Re-raise as a regular die — the parallel loop body errored */
        die("%s", first_error);
        return val_none();
    }

    Value *result = val_list(out);
    list_decref(out);
    return result;
}
```

The worker function (define as `static void *parallel_loop_worker(void *arg)`):

```c
static void *parallel_loop_worker(void *varg) {
    ThreadArg *arg = (ThreadArg *)varg;
    HowFunc   *fn  = arg->fn;

    /* Each thread gets its own fresh local scope */
    Env *local = env_new(arg->closure);

    /* Bind the iteration variable */
    Value *iv = val_num((double)arg->iter_val);
    env_set(local, fn->iter_var, iv);
    val_decref(iv);

    /* Mark this env as a parallel scope — writes to outer scope are errors */
    local->is_parallel = 1;

    Signal sig = {SIG_NONE, NULL};
    run_branches(&fn->branches, local, &sig);

    if (sig.type == SIG_NEXT) sig.type = SIG_NONE;  /* continue — normal */

    if (sig.type == SIG_RETURN && sig.retval) {
        /* Store result at this thread's index slot */
        val_decref(arg->out_items[arg->index]);
        arg->out_items[arg->index] = sig.retval;  /* transfer ownership */
        sig.retval = NULL;
        sig.type   = SIG_NONE;
    }

    if (sig.type != SIG_NONE) {
        /* Unexpected signal (SIG_BREAK, unhandled error) */
        arg->error = xstrdup("unexpected control flow in parallel loop body");
    }

    env_decref(local);
    return NULL;
}
```

### Step 5: Enforce the outer-write restriction

Add `is_parallel` flag to the `Env` struct:
```c
struct Env {
    /* ... existing fields ... */
    int is_parallel;   /* 1 = this is a parallel loop's local scope */
};
```
Initialize to 0 in `env_new()`.

In `env_assign()`, when walking up the parent chain to find an existing binding,
check if we cross a parallel boundary:

```c
static int env_assign(Env *e, const char *key, Value *val) {
    int crossed_parallel = 0;
    for (Env *cur = e; cur; cur = cur->parent) {
        if (cur->is_parallel && cur != e) crossed_parallel = 1;
        for (int i = 0; i < cur->len; i++) {
            if (!strcmp(cur->entries[i].key, key)) {
                if (crossed_parallel) {
                    die("cannot write to outer variable '%s' in a parallel loop", key);
                }
                val_decref(cur->entries[i].val);
                cur->entries[i].val = val_incref(val);
                return 1;
            }
        }
        /* ... instance field check ... */
    }
    return 0;
}
```

This means: if the variable is found in an env that is ABOVE the parallel
boundary, writing to it is an error. Reading (via `env_get`) is fine — no change
needed there.

### Step 6: Add `par(lst, fn)` builtin

In `setup_globals()`:
```c
REG("par", par_fn);
```

The builtin:
```c
BUILTIN(par_fn) {
    NEED(2);
    Value *lst = ARG(0);
    Value *fn  = ARG(1);
    if (lst->type != VT_LIST) die("par() first arg must be a list");

    int n = lst->list->len;
    if (n == 0) {
        HowList *empty = list_new();
        Value *v = val_list(empty); list_decref(empty); return v;
    }

    /* Build a synthetic LOOP_PARALLEL HowFunc and call run_parallel_loop */
    /* Simpler: just build args and call fn in parallel directly */
    /* ... see implementation note below ... */
}
```

Implementation note: `par()` can be implemented by building a synthetic
`HowFunc` with `LOOP_PARALLEL`, or more directly by reusing the thread pool
logic inline. The cleanest approach is to extract the thread pool into a
`run_parallel_apply(Value **items, int n, Value *fn)` helper that both
`run_parallel_loop` and `par_fn` call.

### Step 7: Compile with `-lpthread`

Update the build line:
```
cc -O2 -o howlang howlang.c -lm -lpthread
```

Update the readme build instructions accordingly.

---

## Thread Safety Considerations

**GC:** The GC (`gc_collect`) uses global linked lists and must not run during
parallel execution. Options:
1. Disable GC during parallel loops (simplest — parallel loops are short-lived)
2. Use a mutex around `gc_collect`
3. Each thread uses its own allocation arena (complex)

Recommended: option 1. Add a `g_gc_suspended` global flag. Set it before
launching threads, clear it after joining. `gc_collect` checks the flag and
returns immediately if set.

**Value allocation:** `val_new()`, `map_new()`, `list_new()` write to the global
GC linked lists (`g_all_values` etc.). With GC suspended, these lists won't be
collected, but concurrent writes would corrupt them. Options:
1. Use a mutex around all allocations (simple, some contention)
2. Each thread pre-allocates values before threading (hard)
3. Each thread uses thread-local storage for new allocations (complex)

Recommended: option 1. A single `pthread_mutex_t g_alloc_mutex` protecting
`val_new`, `map_new`, `list_new`, `list_push`, `map_set`, `env_new`.
Lock only during the allocation/write, not during the computation.

**The closure env (`fn->closure`):** Threads read from the shared closure but
the outer-write restriction prevents them from writing to it. Read-only access
to a shared env is safe without a mutex as long as no thread writes to it.
The `is_parallel` restriction enforces this.

**Result list:** Each thread writes to `out->items[arg->index]` — a unique
index — so no mutex is needed for result collection.

---

## What Does NOT Need to Change

- The sequential for-range `(i=0:n){ }()` — completely unchanged
- The unbounded loop `(:){ }()` — completely unchanged  
- All existing test files — no changes needed
- The `break` and `continue` semantics in sequential loops — unchanged
- The GC, environments, value system — unchanged except the `is_parallel` flag
  and the allocation mutex

---

## Non-Goals

This proposal deliberately does NOT implement:

- Async/await or futures
- Channels or message passing
- Automatic parallelism detection (the programmer marks what's parallel)
- Work stealing or adaptive thread pool sizing
- Nested parallel loops (a `^{ }` body cannot contain another `^{ }`)
- Shared mutable state between iterations (the write restriction prevents this)
