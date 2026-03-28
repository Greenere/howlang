# Howlang — `grad()` Automatic Differentiation Proposal

## Context

Howlang is a small language where everything is a function. This proposal
adds automatic differentiation as a first-class language feature: `grad(f)`
takes any function and returns a new function that computes the gradient of `f`.

The design fits the existing model: `grad` is a builtin higher-order function,
the result is a plain callable value, and every function can optionally declare
its own backward pass using a `grad` block.

Build with:
```
cc -O2 -o howlang howlang.c -lm
```

All test suites must pass after every step:
```
./howlang samples/test_all.how          # 54/54
cd samples && ../howlang graph_test.how       # 32/32
cd samples && ../howlang lru_cache_test.how   # 34/34
cd samples && ../howlang brainfuck_test.how   # 32/32
./howlang test_loops.how                # 41/41
```

---

## What `grad()` Does

`grad(f)` returns a new function with the same calling convention as `f` but
computes the gradient of `f`'s output with respect to its inputs:

```
var f = (x){ :: x * x + 3 * x }
var df = grad(f)
df(3.0)          # 9.0  (= 2*3 + 3)
grad(grad(f))(3.0)  # 2.0  (second derivative)
```

`grad(f)` **always has the same calling convention as `f`**. What changes is
the return type: instead of the value, you get its gradient structure.

| `f` signature | `grad(f)` signature | example |
|---|---|---|
| `number → number` | `number → number` | `f(x) = x²`, `grad(f)(3) = 6` |
| `list → number` | `list → list` | gradient vector, same shape as input |
| `() → number` | `() → map` | gradients of all closed-over numeric vars |
| `(a,b) → number` | `(a,b) → list` | `{∂f/∂a, ∂f/∂b}` |
| `anything → non-number` | `anything → none` | not differentiable, no error |

### The ML training case: zero-argument closure

For neural networks, the function to differentiate closes over the weights.
Wrap the forward pass and loss in a zero-argument closure:

```
var loss_fn = (){
    var h1 = l1.forward(x),
    var h2 = l2.forward(h1),
    var y  = l3.forward(h2)(0),
    :: (y - target) * (y - target)
}

var grads = grad(loss_fn)()
# grads is a map: {W1: ..., b1: ..., W2: ..., b2: ..., W3: ..., b3: ...}
# keyed by every closed-over numeric variable that participated in the computation
```

`grad(loss_fn)()` runs the forward pass with a tape recording all operations,
then propagates gradients backward. The entire manual backward pass — `relu_grad`,
`sigmoid_grad`, `matvec_T`, `outer`, `matsub` — is replaced by this one call.

---

## New Syntax: `grad` Block

Functions can optionally declare a custom backward pass. This handles
non-numeric functions and custom gradient rules:

```
var f = (params){ :: forward_expr } grad (params, g){ :: backward_expr }
```

`params` are the same parameter names as the forward function. `g` is the
upstream gradient (how much the output affects the loss). The block returns
the downstream gradient (in the same shape as the input).

```
# relu — auto-differentiation via tape works fine, but explicit is also valid:
var relu = (x){ x > 0 :: x, :: 0 } grad (x, g){ x > 0 :: g, :: 0 }

# Embedding lookup — non-numeric index, custom gradient:
var embed = (idx){
    :: table(idx)
} grad (idx, g){
    table(idx) -= lr * g,   # sparse gradient update
    :: none                  # no gradient w.r.t. discrete index
}
```

**Without a `grad` block:** the runtime uses tape-based reverse-mode AD
automatically for numeric outputs.

**With a `grad` block:** the runtime calls it instead of the tape. This is
useful for custom ops, non-differentiable operations with known sensitivities,
and performance-critical paths where the derivative is known analytically.

---

## Implementation

### Step 1: `VT_DUAL` — the dual number value type

Add a new value type to the `VT` enum:

```c
typedef enum {
    VT_NONE, VT_BOOL, VT_NUM, VT_STR,
    VT_LIST, VT_MAP,
    VT_FUNC, VT_CLASS, VT_INSTANCE, VT_MODULE,
    VT_BUILTIN,
    VT_DUAL,    /* dual number for forward-mode AD: {val, tan} */
} VT;
```

Add to the `Value` union:

```c
struct { double val; double tan; } dual;  /* VT_DUAL */
```

Add to the GC linked list (same pattern as all other types):

```c
static Value *g_all_duals = NULL;   /* or reuse g_all_values since Value is already tracked */
```

Since `VT_DUAL` is just a `Value` with two doubles, it can use the existing
`g_all_values` GC list. No separate tracking needed.

Add constructor:
```c
static Value *val_dual(double v, double t) {
    Value *d = val_new(VT_DUAL);
    d->dual.val = v;
    d->dual.tan = t;
    return d;
}
```

Add to `val_repr`:
```c
case VT_DUAL: {
    char buf[64];
    snprintf(buf, sizeof(buf), "<dual %.6g + %.6gε>", v->dual.val, v->dual.tan);
    return xstrdup(buf);
}
```

### Step 2: Operator overloading for `VT_DUAL` in `eval` `N_BINOP`

In the `N_BINOP` eval case, before the existing type checks, add VT_DUAL
handling. This is the forward-mode AD chain rule, hard-coded per operator.

Add a helper (define before `eval`):

```c
/* Forward-mode dual arithmetic — applies the chain rule for each op */
static Value *dual_binop(Value *l, Value *r, const char *op, int line) {
    /* Promote plain numbers to dual with zero tangent */
    double lv = (l->type==VT_DUAL) ? l->dual.val : l->nval;
    double lt = (l->type==VT_DUAL) ? l->dual.tan : 0.0;
    double rv = (r->type==VT_DUAL) ? r->dual.val : r->nval;
    double rt = (r->type==VT_DUAL) ? r->dual.tan : 0.0;

    double out_v, out_t;

    if (!strcmp(op,"+")) { out_v = lv+rv; out_t = lt+rt; }
    else if (!strcmp(op,"-")) { out_v = lv-rv; out_t = lt-rt; }
    else if (!strcmp(op,"*")) { out_v = lv*rv; out_t = lt*rv + lv*rt; }   /* product rule */
    else if (!strcmp(op,"/")) {
        if (rv==0) die_at(line, 0, "division by zero");
        out_v = lv/rv;
        out_t = (lt*rv - lv*rt) / (rv*rv);   /* quotient rule */
    }
    else if (!strcmp(op,"%")) { out_v = fmod(lv,rv); out_t = lt; }  /* approx */
    /* Comparisons: return bool based on primal value, tangent is irrelevant */
    else if (!strcmp(op,"==")) return val_bool(lv==rv);
    else if (!strcmp(op,"!=")) return val_bool(lv!=rv);
    else if (!strcmp(op,"<"))  return val_bool(lv<rv);
    else if (!strcmp(op,">"))  return val_bool(lv>rv);
    else if (!strcmp(op,"<=")) return val_bool(lv<=rv);
    else if (!strcmp(op,">=")) return val_bool(lv>=rv);
    else die_at(line, 0, "unknown op '%s' on dual numbers", op);

    return val_dual(out_v, out_t);
}
```

In `eval N_BINOP`, at the top of the binop dispatch (before the existing
`VT_NUM + VT_NUM` checks), add:

```c
/* Dual number arithmetic — forward-mode automatic differentiation */
if (l->type==VT_DUAL || r->type==VT_DUAL) {
    /* Only valid when both sides are numeric (num or dual) */
    if ((l->type==VT_NUM||l->type==VT_DUAL) &&
        (r->type==VT_NUM||r->type==VT_DUAL)) {
        Value *res = dual_binop(l, r, op, node->line);
        GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
        val_decref(l); val_decref(r);
        return res;
    }
}
```

Also handle `VT_DUAL` in unary `-`:
```c
if (v->type==VT_DUAL) {
    Value *res = val_dual(-v->dual.val, -v->dual.tan);
    GC_UNROOT_VALUE(); val_decref(v); return res;
}
```

And in `how_truthy` (for branching on dual numbers):
```c
if (v->type==VT_DUAL) return v->dual.val != 0.0;
```

And `how_eq`:
```c
if (a->type==VT_DUAL && b->type==VT_DUAL) return a->dual.val==b->dual.val;
if (a->type==VT_DUAL && b->type==VT_NUM)  return a->dual.val==b->nval;
if (a->type==VT_NUM  && b->type==VT_DUAL) return a->nval==b->dual.val;
```

### Step 3: Dual-aware builtins

The numeric builtins (`abs`, `sqrt`, `floor`, `ceil`, `max`, `min`) need to
handle `VT_DUAL` inputs. Each applies the chain rule for its operation:

```c
/* In builtin_abs_fn: */
if (ARG(0)->type==VT_DUAL) {
    double v = ARG(0)->dual.val, t = ARG(0)->dual.tan;
    return val_dual(fabs(v), v >= 0 ? t : -t);  /* d|x|/dx = sign(x) */
}

/* In builtin_sqrt_fn: */
if (ARG(0)->type==VT_DUAL) {
    double v = ARG(0)->dual.val, t = ARG(0)->dual.tan;
    if (v < 0) die_at(0, 0, "sqrt of negative dual number");
    double sv = sqrt(v);
    return val_dual(sv, t / (2.0 * sv));   /* d sqrt(x)/dx = 1/(2√x) */
}

/* In builtin_floor_fn and ceil_fn: */
if (ARG(0)->type==VT_DUAL) {
    return val_dual(floor(ARG(0)->dual.val), 0.0);  /* derivative is 0 a.e. */
}
```

### Step 4: `grad()` builtin — forward mode (single input)

This implements `grad(f)` using dual numbers. Returns a new function.

```c
/* A VT_FUNC wrapping the grad operation — stores the primal function */
/* We create a synthetic builtin that closes over the primal */

/* Helper: compute grad of f at a single numeric argument */
static Value *compute_grad_scalar(Value *fn, Value *arg, Signal *sig) {
    /* Wrap arg as dual(arg, 1.0) — seed = 1 for "differentiate w.r.t. this" */
    Value *dual_arg = val_dual(arg->nval, 1.0);
    GC_ROOT_VALUE(dual_arg);
    Value *result = eval_call_val(fn, &dual_arg, 1, sig, 0);
    GC_UNROOT_VALUE();
    val_decref(dual_arg);
    if (sig->type != SIG_NONE) return result;

    /* Extract gradient from dual result */
    if (result->type == VT_DUAL) {
        double grad = result->dual.tan;
        val_decref(result);
        return val_num(grad);
    }
    /* Non-numeric output: return none */
    val_decref(result);
    return val_none();
}
```

The `grad` builtin itself:

```c
BUILTIN(grad_fn) {
    NEED(1);
    Value *f = ARG(0);
    if (f->type != VT_FUNC && f->type != VT_BUILTIN)
        die("grad() requires a function");

    /* Return a new builtin that, when called, computes the gradient of f */
    /* We need to close over f — use a map to store it, then a builtin wraps it */
    /* Simplest approach: create a VT_FUNC wrapper that holds f in its closure */

    /* Create a closure env containing the primal function */
    Env *grad_env = env_new(NULL);
    GC_ROOT_ENV(grad_env);
    env_set(grad_env, "__primal__", f);

    /* The grad wrapper function body will be evaluated specially */
    /* We use a sentinel: a VT_FUNC with is_grad=1 and a closure containing __primal__ */
    HowFunc *gfn = xmalloc(sizeof(*gfn));
    memset(gfn, 0, sizeof(*gfn));
    gfn->closure  = grad_env; grad_env->refcount++;
    gfn->is_grad  = 1;   /* new flag — see Step 5 */
    gfn->refcount = 1;
    gfn->gc_next  = g_all_funcs; g_all_funcs = gfn; g_gc_allocations++;

    GC_UNROOT_ENV();
    env_decref(grad_env);

    Value *v = val_new(VT_FUNC);
    v->func = gfn;
    return v;
}
```

### Step 5: Add `is_grad` flag to `HowFunc` and dispatch in `eval_call_val`

Add to `HowFunc`:
```c
struct HowFunc {
    StrList  params;
    NodeList branches;
    Env     *closure;
    int      is_loop;
    int      is_forrange;
    char    *iter_var;
    Node    *fr_start;
    Node    *fr_stop;
    int      is_grad;       /* 1 = this is a grad wrapper, closure has __primal__ */
    HowFunc *grad_fn;       /* custom grad block if defined (Step 8) */
    int      refcount;
    int      gc_mark;
    struct HowFunc *gc_next;
};
```

In `eval_call_val`, before the regular function dispatch, add:

```c
if (fn->is_grad) {
    Value *primal = env_get(fn->closure, "__primal__");
    if (!primal) die("grad wrapper lost its primal function");

    /* Check if primal has a custom grad block */
    if (primal->type==VT_FUNC && primal->func->grad_fn) {
        return call_custom_grad(primal->func, args, argc, sig, line);
    }

    /* Forward-mode: single numeric argument */
    if (argc == 1 && args[0]->type == VT_NUM) {
        return compute_grad_scalar(primal, args[0], sig);
    }

    /* Multiple numeric arguments: return list of partial derivatives */
    if (argc > 1) {
        HowList *grads = list_new();
        int all_numeric = 1;
        for (int i = 0; i < argc; i++) {
            if (args[i]->type != VT_NUM) { all_numeric = 0; break; }
        }
        if (all_numeric) {
            for (int i = 0; i < argc; i++) {
                /* Seed the i-th argument, zero the rest */
                Value **dual_args = xmalloc(argc * sizeof(Value*));
                for (int j = 0; j < argc; j++) {
                    dual_args[j] = (j==i)
                        ? val_dual(args[j]->nval, 1.0)
                        : val_dual(args[j]->nval, 0.0);
                }
                Value *res = eval_call_val(primal, dual_args, argc, sig, line);
                for (int j = 0; j < argc; j++) val_decref(dual_args[j]);
                free(dual_args);
                if (sig->type != SIG_NONE) { list_decref(grads); return res; }
                double g = (res->type==VT_DUAL) ? res->dual.tan : 0.0;
                val_decref(res);
                Value *gv = val_num(g);
                list_push(grads, gv); val_decref(gv);
            }
            Value *result = val_list(grads); list_decref(grads); return result;
        }
    }

    /* Zero-argument closure: reverse-mode AD (Step 6) */
    if (argc == 0) {
        return compute_grad_closure(primal, sig);
    }

    /* Non-numeric or unhandled: return none */
    return val_none();
}
```

Register the builtin in `setup_globals`:
```c
REG("grad", grad_fn);
```

### Step 6: `compute_grad_closure()` — reverse-mode AD for zero-arg closures

This is the ML training case: `grad(loss_fn)()` where `loss_fn` closes over
weight tensors. Returns a map of `{variable_name: gradient_value}`.

Reverse-mode requires a tape. Add a global tape structure:

```c
/* Tape entry: records one operation during the forward pass */
typedef struct TapeEntry {
    int    out_id;      /* index of output value in tape_values[] */
    char   op[8];       /* operation: "+", "-", "*", "/", "neg" */
    int    in_ids[2];   /* input value indices (-1 if not a tape value) */
    double in_vals[2];  /* primal values of inputs (for VJP rules) */
} TapeEntry;

#define TAPE_MAX_ENTRIES 65536
#define TAPE_MAX_VALUES  65536

static TapeEntry  g_tape[TAPE_MAX_ENTRIES];
static int        g_tape_len = 0;
static double     g_tape_values[TAPE_MAX_VALUES];   /* primal values */
static double     g_tape_grads[TAPE_MAX_VALUES];    /* accumulated gradients */
static int        g_tape_next_id = 0;
static int        g_tape_active = 0;   /* 1 = currently recording */
```

When `g_tape_active` is set, replace `VT_DUAL` numbers with tape-tracked
values. Add a new `VT_TRACKED` value type (or reuse VT_DUAL with a tape index
stored in the `tan` field as an integer ID):

```c
/* Use VT_DUAL with tan = -(tape_id + 1) to signal "this is a tape value" */
/* Negative tan means: tape value at index (-tan - 1) */
/* This avoids adding another VT_ enum entry */
#define IS_TAPE_VAL(v) ((v)->type==VT_DUAL && (v)->dual.tan < -0.5)
#define TAPE_ID(v)     ((int)(-(v)->dual.tan) - 1)

static Value *tape_new_val(double primal) {
    int id = g_tape_next_id++;
    if (id >= TAPE_MAX_VALUES) die("tape overflow");
    g_tape_values[id] = primal;
    g_tape_grads[id]  = 0.0;
    return val_dual(primal, -(double)(id + 1));
}
```

During `dual_binop` (called when `g_tape_active`), record to the tape:

```c
if (g_tape_active) {
    /* Record the operation */
    TapeEntry *e = &g_tape[g_tape_len++];
    int out_id = g_tape_next_id;
    e->out_id = out_id;
    strncpy(e->op, op, 7);
    e->in_ids[0] = IS_TAPE_VAL(l) ? TAPE_ID(l) : -1;
    e->in_ids[1] = IS_TAPE_VAL(r) ? TAPE_ID(r) : -1;
    e->in_vals[0] = (l->type==VT_DUAL) ? l->dual.val : l->nval;
    e->in_vals[1] = (r->type==VT_DUAL) ? r->dual.val : r->nval;
    return tape_new_val(out_v);   /* out_t encodes tape id */
}
```

The VJP (vector-Jacobian product) rules for the backward pass:

```c
static void tape_backward(int out_id, double grad) {
    /* Walk the tape in reverse from out_id */
    for (int i = g_tape_len - 1; i >= 0; i--) {
        TapeEntry *e = &g_tape[i];
        if (e->out_id != out_id &&
            g_tape_grads[e->out_id] == 0.0) continue;   /* prune zero grads */

        double g = g_tape_grads[e->out_id];
        double lv = e->in_vals[0], rv = e->in_vals[1];
        double gl = 0.0, gr = 0.0;

        if      (!strcmp(e->op,"+"))  { gl = g;          gr = g; }
        else if (!strcmp(e->op,"-"))  { gl = g;          gr = -g; }
        else if (!strcmp(e->op,"*"))  { gl = g * rv;     gr = g * lv; }
        else if (!strcmp(e->op,"/"))  { gl = g / rv;     gr = -g * lv / (rv*rv); }
        else if (!strcmp(e->op,"neg"))  { gl = -g; }

        if (e->in_ids[0] >= 0) g_tape_grads[e->in_ids[0]] += gl;
        if (e->in_ids[1] >= 0) g_tape_grads[e->in_ids[1]] += gr;
    }
}
```

`compute_grad_closure`:

```c
static Value *compute_grad_closure(Value *primal_fn, Signal *sig) {
    HowFunc *fn = primal_fn->func;

    /* Reset tape */
    g_tape_len = 0; g_tape_next_id = 0;
    memset(g_tape_grads, 0, sizeof(double) * TAPE_MAX_VALUES);

    /* Seed closed-over numeric vars as tape values */
    /* Walk the closure env, replace each VT_NUM with a VT_DUAL tape value */
    /* Track: {var_name → tape_id} for gradient retrieval after backward */
    HowMap *var_to_id = map_new();   /* var_name → tape_id as string */

    Env *env = fn->closure;
    while (env) {
        for (int i = 0; i < env->len; i++) {
            if (env->entries[i].val->type == VT_NUM) {
                int id = g_tape_next_id;
                Value *tv = tape_new_val(env->entries[i].val->nval);
                /* Replace the var's value with the tape value */
                val_decref(env->entries[i].val);
                env->entries[i].val = tv;
                /* Record the mapping */
                char id_str[32]; snprintf(id_str, sizeof(id_str), "%d", id);
                Value *id_val = val_str(id_str);
                map_set(var_to_id, env->entries[i].key, id_val);
                val_decref(id_val);
            }
        }
        env = env->parent;
    }

    /* Run the forward pass with tape recording */
    g_tape_active = 1;
    Value *result = eval_call_val(primal_fn, NULL, 0, sig, 0);
    g_tape_active = 0;

    if (sig->type != SIG_NONE || result->type != VT_DUAL) {
        /* Restore vars and return none */
        /* (restoration logic omitted for brevity — see note below) */
        val_decref(result); map_decref(var_to_id);
        return val_none();
    }

    /* Backward pass: seed the output gradient = 1.0 */
    int out_id = TAPE_ID(result);
    g_tape_grads[out_id] = 1.0;
    val_decref(result);

    /* Actually walk the tape backward */
    for (int i = g_tape_len - 1; i >= 0; i--) {
        TapeEntry *e = &g_tape[i];
        double g = g_tape_grads[e->out_id];
        if (g == 0.0) continue;

        double lv = e->in_vals[0], rv = e->in_vals[1];
        double gl = 0.0, gr = 0.0;
        if      (!strcmp(e->op,"+"))  { gl = g;      gr = g; }
        else if (!strcmp(e->op,"-"))  { gl = g;      gr = -g; }
        else if (!strcmp(e->op,"*"))  { gl = g * rv; gr = g * lv; }
        else if (!strcmp(e->op,"/"))  { gl = g / rv; gr = -g * lv / (rv*rv); }
        else if (!strcmp(e->op,"neg")){ gl = -g; }

        if (e->in_ids[0] >= 0) g_tape_grads[e->in_ids[0]] += gl;
        if (e->in_ids[1] >= 0) g_tape_grads[e->in_ids[1]] += gr;
    }

    /* Collect gradients: build result map {var_name: gradient} */
    HowMap *grad_map = map_new();
    char **keys_arr = xmalloc(var_to_id->len * sizeof(char*));
    for (int i = 0; i < var_to_id->len; i++) {
        const char *vname = var_to_id->pairs[i].key;
        const char *id_str = var_to_id->pairs[i].val->sval;
        int id = atoi(id_str);
        Value *gval = val_num(g_tape_grads[id]);
        map_set(grad_map, vname, gval);
        val_decref(gval);
    }

    /* Restore original numeric values in the closure */
    env = fn->closure;
    while (env) {
        for (int i = 0; i < env->len; i++) {
            if (IS_TAPE_VAL(env->entries[i].val)) {
                double primal = env->entries[i].val->dual.val;
                val_decref(env->entries[i].val);
                env->entries[i].val = val_num(primal);
            }
        }
        env = env->parent;
    }

    map_decref(var_to_id);

    Value *result_map = val_map(grad_map); map_decref(grad_map);
    return result_map;
}
```

**Implementation note on closure mutation:** The above temporarily replaces
closed-over VT_NUM values with tape values during the forward pass, then
restores them. A cleaner alternative is to create a copy of the closure env
for the tape pass and leave the original untouched.

### Step 7: Parse the `grad` block

The `grad` block attaches a custom backward function to any function at
definition time:

```
var f = (x){ :: x * x } grad (x, g){ :: 2 * x * g }
```

**Parser change:** In `parse_atom`, after parsing a function body (both the
`(){}` and `[]{}` paths), check for the `grad` keyword:

```c
/* In parse_atom, after any complete function parse: */
if (p_check(p, TT_IDENT) &&
    strcmp(p_peek(p,0)->sval, "grad") == 0 &&
    p_peek(p,1)->type == TT_LPAREN) {
    p_adv(p);  /* consume 'grad' */
    /* Parse the grad block as a regular function */
    Node *grad_node = parse_atom(p);   /* (params){ body } */
    n->func.grad_body = grad_node;     /* new field on N_FUNC */
}
```

Add `grad_body` to the `N_FUNC` node:
```c
struct { /* N_FUNC, N_CLASS */
    StrList  params;
    NodeList branches;
    LoopKind loop_kind;
    char    *iter_var;
    Node    *loop_start;
    Node    *loop_stop;
    Node    *grad_body;    /* optional custom backward pass */
} func;
```

**Evaluator change:** When evaluating an `N_FUNC` node, if `grad_body` is
present, compile it as a separate `HowFunc` and store it in `gfn->grad_fn`:

```c
case N_FUNC: {
    HowFunc *fn = xmalloc(sizeof(*fn));
    /* ... existing init ... */
    fn->grad_fn = NULL;
    if (node->func.grad_body) {
        /* Compile the grad block as a function */
        /* It takes (original_params..., g) as arguments */
        Signal inner = {SIG_NONE, NULL};
        Value *grad_val = eval(node->func.grad_body, env, &inner);
        if (grad_val->type == VT_FUNC) {
            fn->grad_fn = grad_val->func;
            fn->grad_fn->refcount++;
        }
        val_decref(grad_val);
    }
    /* ... rest of existing init ... */
}
```

**`call_custom_grad`:** When a function with a `grad_fn` is differentiated:

```c
static Value *call_custom_grad(HowFunc *fn, Value **args, int argc,
                               Signal *sig, int line) {
    /* Need: the primal's args + the upstream gradient g */
    /* For now: call the primal first to get the forward value */
    Value *primal_result = eval_call_val_fn(fn, args, argc, sig, line);
    if (sig->type != SIG_NONE) return primal_result;

    /* The upstream gradient: for scalar output, it's 1.0 */
    Value *g = val_num(1.0);

    /* Build args for grad_fn: original args + g */
    Value **grad_args = xmalloc((argc + 1) * sizeof(Value*));
    for (int i = 0; i < argc; i++) grad_args[i] = args[i];
    grad_args[argc] = g;

    Value *grad_result = eval_call_val(
        /* wrap fn->grad_fn as a Value temporarily */,
        grad_args, argc + 1, sig, line);

    val_decref(g);
    val_decref(primal_result);
    free(grad_args);
    return grad_result;
}
```

### Step 8: GC — mark and sweep for `VT_DUAL`

In `gc_mark_value`:
```c
case VT_DUAL: break;  /* no sub-values to mark, just the two doubles */
```

In `gc_sweep_values` — no special handling needed since `VT_DUAL` values are
in `g_all_values` already.

In the `HowFunc` GC sweep, mark `grad_fn` if present:
```c
static void gc_mark_func(HowFunc *f) {
    if (!f || f->gc_mark) return;
    f->gc_mark = 1;
    gc_mark_env(f->closure);
    if (f->grad_fn) gc_mark_func(f->grad_fn);
}
```

---

## Verification Tests

Add to `test_all.how` or a new `test_grad.how`:

```
# Basic scalar derivative
var f = (x){ :: x * x }
assert_eq("grad x^2 at 3",    grad(f)(3.0),  6.0)
assert_eq("grad x^2 at 0",    grad(f)(0.0),  0.0)
assert_eq("grad x^2 at -2",   grad(f)(-2.0), -4.0)

# Higher-order
var d2f = grad(grad(f))
assert_eq("grad^2 x^2 at 3",  d2f(3.0), 2.0)

# Multivariate
var g = (x, y){ :: x * x + x * y }
var dg = grad(g)
# dg(2, 3) = {∂g/∂x, ∂g/∂y} = {2*2+3, 2} = {7, 2}
var grads = dg(2.0, 3.0)
assert_eq("∂/∂x at (2,3)",  grads(0), 7.0)
assert_eq("∂/∂y at (2,3)",  grads(1), 2.0)

# Branching: relu
var relu = (x){ x > 0 :: x, :: 0 }
assert_eq("grad relu at  3",  grad(relu)(3.0),   1.0)
assert_eq("grad relu at -1",  grad(relu)(-1.0),  0.0)

# Non-numeric: returns none
var label = (x){ x > 0 :: "pos", :: "neg" }
assert_eq("grad of non-numeric", grad(label)(1.0), none)

# Custom grad block
var abs_custom = (x){ x >= 0 :: x, :: -x } grad (x, g){ x >= 0 :: g, :: -g }
assert_eq("custom grad abs at  3",  grad(abs_custom)(3.0),   1.0)
assert_eq("custom grad abs at -2",  grad(abs_custom)(-2.0), -1.0)

# Closure gradient (zero-arg)
var a = 3.0
var b = 2.0
var h = (){ :: a * a + b }
var gs = grad(h)()
assert_eq("∂/∂a of a^2+b", gs("a"), 6.0)   # 2*a = 6
assert_eq("∂/∂b of a^2+b", gs("b"), 1.0)   # constant
```

---

## What Does NOT Change

- All existing syntax — no tokens removed or repurposed
- The `'` single-quote string delimiter — completely unchanged
- All 5 test suites — must pass without modification
- The branch model, class system, loop forms — untouched
- Performance of non-gradient code — `g_tape_active` is 0 by default,
  `VT_DUAL` check in binop only fires if one operand is actually dual

---

## Known Limitations of This Implementation

1. **Tape size is fixed** — `TAPE_MAX_ENTRIES = 65536` is enough for the
   MLP example but not for large networks. Make it dynamic (realloc) for
   production use.

2. **Tensor support** — this proposal covers scalar and list-of-scalars
   gradients. For full tensor support (matrix operations), extend the tape
   to record tensor ops and implement the matrix VJP rules (`W^T @ grad`,
   `grad @ x^T` for matmul).

3. **Closure mutation** — `compute_grad_closure` temporarily mutates the
   closure's numeric values to tape values. If the closure is shared or
   called concurrently, this is unsafe. A clean implementation copies the
   closure env for the tape pass.

4. **In-place weight updates** — after `grad(loss_fn)()`, the weights need
   to be updated: `W1 -= lr * grads("W1")`. With tensor support, `grads("W1")`
   would be a tensor of the same shape as `W1`.
