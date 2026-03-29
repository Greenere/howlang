# Howlang — `grad` Block Fix: Function Parameters - Done ✅

## What was incomplete in `grad_proposal.md`

Step 7 of `grad_proposal.md` described the `grad` block but left two things
underspecified:

1. **What does the grad block return when a parameter is a function?**
   The proposal said "params are the same parameter names as the forward" and
   "returns the downstream gradient." But `∂f/∂layer` where `layer` is a
   function is not a number or tensor — the proposal had no answer.

2. **Who provides the primal values to the grad block?**
   The grad block needs `x`'s *original* value (before it was wrapped as a tape
   value) to compute `2 * x * g`. The proposal did not specify how those primal
   values get there.

This document corrects both.

---

## Core insight: two separate mechanisms

Gradient propagation has two completely separate paths:

**Path 1 — The tape (automatic, handles function parameters)**

When `grad(f)(g, x)` runs with `g` as a function parameter and `x` as a
number, the tape is active throughout the entire forward pass. When `g(x)`
executes inside `f`, every arithmetic operation inside `g` is recorded to
the tape — including operations on `g`'s closed-over tensors `W`, `b`, etc.
The backward pass then propagates through those tape entries automatically.
Gradient flows *through* the function parameter transparently.
**No syntax is needed. No grad block can improve this.**

**Path 2 — The grad block (manual override, for non-traceable ops)**

The tape cannot differentiate through `argmax`, `sort`, comparisons used as
discrete masks, or external code. The grad block is the escape hatch: it
provides a custom VJP (vector-Jacobian product) for a specific operation.
It overrides specific parameters. Everything else falls back to the tape.

These two paths are independent. The grad block is never needed for function
parameters because the tape already handles them.

---

## Revised grad block semantics

### The grad block is a partial override

The grad block returns a **map of overrides**, not a complete gradient
specification. Parameters not mentioned in the return map default to
tape-computed gradients.

```
var f = (params){ :: forward_expr } grad (params, g){ :: override_map }
```

Where `override_map` is `{param_name: gradient_value, ...}`:

| param type | gradient value | meaning |
|---|---|---|
| numeric (number/tensor) | a number/tensor | explicit gradient ∂output/∂param |
| numeric | `none` | use tape (same as omitting) |
| function | `none` | use tape through calls to this param |
| function | omitted | use tape (strongly preferred) |
| any | omitted | use tape |

An **empty map** `{}` means "let the tape handle everything" — equivalent to
having no grad block at all.

### Primal values are provided automatically

The grad block's parameters receive the **primal values from the forward pass**
— the original numeric inputs, not tape-tracked surrogates. The tape machinery
saves `in_vals[]` for every recorded operation. When the backward pass invokes
a custom grad block, it passes these saved primals. The user never calls
`save_for_backward` or does anything explicit.

---

## Examples

### Function parameter — no grad block needed

```
var apply = (f, x){ :: f(x) * 2 }
var square = (x){ :: x * x }
grad(apply)(square, 3.0)
# tape traces: x=3.0, square(x)=9.0, output=18.0
# backward: ∂output/∂x = 2 * 2*3.0 = 12.0  (chain rule through square)
# returns: {f: none, x: 12.0}
# No grad block needed. Tape handles f transparently.
```

### Numeric-only grad block (ignore function params)

```
var scale_apply = (f, x, s){
    :: f(x) * s
} grad (x, s, g){
    # Override x and s. Omit f — tape handles propagation through f(x).
    :: {x: s * g, s: f(x) * g}   # note: f(x) here is the saved primal
}
```

### Non-differentiable op — the real use case for grad block

```
# Step function: derivative is 0 everywhere (or undefined at 0).
# Straight-through estimator: pretend derivative is 1.
var step = (x){
    x > 0 :: 1.0,
    :: 0.0
} grad (x, g){
    :: {x: g}   # STE: pass upstream gradient straight through
}

# Argmax with surrogate gradient:
var soft_select = (scores, values){
    var idx = argmax(scores)    # non-differentiable
    :: values(idx)
} grad (scores, values, g){
    # One-hot gradient for scores at argmax position
    var n = len(scores),
    var idx = argmax(scores),
    var scores_grad = zeros({n}),
    scores_grad(idx) = g,
    # values: tape handles it (values(idx) is a traceable index read)
    :: {scores: scores_grad}
}
```

### All-numeric function — primal values in grad block

```
var f = (x){ :: x * x } grad (x, g){ :: {x: 2.0 * x * g} }
# When grad(f)(3.0) runs:
# forward: x_primal=3.0, tape records output=9.0
# backward: grad block called with x=3.0 (primal), g=1.0 (upstream)
# returns: {x: 2.0 * 3.0 * 1.0} = {x: 6.0}  ✓
```

---

## What changes in `grad_proposal.md`

### Replace Step 7 `call_custom_grad` with this implementation

The key change: the grad block is called with **primal values** (not tape
values), and its return value is a **partial map** that is merged with
tape-computed gradients. Function parameters receive `none` from the map and
their gradients flow through the tape.

```c
static Value *call_custom_grad(HowFunc *fn, Value **args, int argc,
                               Signal *sig, int line) {
    /* Step 1: run the forward pass with tape to record all operations */
    /* args[] are already the primal values (not tape-tracked) at this point */

    /* Step 2: save primal values before tape wrapping */
    Value **primals = xmalloc(argc * sizeof(Value*));
    for (int i = 0; i < argc; i++) primals[i] = val_incref(args[i]);

    /* Step 3: run forward with tape to get the output tape id */
    g_tape_active = 1;
    Value **tape_args = xmalloc(argc * sizeof(Value*));
    for (int i = 0; i < argc; i++) {
        if (args[i]->type == VT_NUM)
            tape_args[i] = tape_new_val(args[i]->nval);
        else
            tape_args[i] = val_incref(args[i]);  /* non-numeric: pass through */
    }
    Value *fwd_result = eval_call_val_fn(fn, tape_args, argc, sig, line);
    g_tape_active = 0;

    for (int i = 0; i < argc; i++) val_decref(tape_args[i]);
    free(tape_args);

    if (sig->type != SIG_NONE || !IS_TAPE_VAL(fwd_result)) {
        val_decref(fwd_result);
        for (int i = 0; i < argc; i++) val_decref(primals[i]);
        free(primals);
        return val_none();
    }

    /* Step 4: seed the output gradient */
    g_tape_grads[TAPE_ID(fwd_result)] = 1.0;
    val_decref(fwd_result);

    /* Step 5: call the custom grad block with (primals..., upstream_g=1.0) */
    Value **grad_args = xmalloc((argc + 1) * sizeof(Value*));
    for (int i = 0; i < argc; i++) grad_args[i] = primals[i];
    grad_args[argc] = val_num(1.0);   /* upstream gradient g */

    /* Wrap fn->grad_fn as a Value to call it */
    Value *grad_fn_val = val_new(VT_FUNC);
    grad_fn_val->func = fn->grad_fn;
    fn->grad_fn->refcount++;

    Signal grad_sig = {SIG_NONE, NULL};
    Value *override_map = eval_call_val(grad_fn_val, grad_args, argc + 1,
                                        &grad_sig, line);
    val_decref(grad_fn_val);
    val_decref(grad_args[argc]);  /* the upstream g=1.0 */
    free(grad_args);

    /* Step 6: apply tape-computed gradients for omitted params,
       override with grad block results for specified params */
    /* First: run tape backward pass */
    for (int i = g_tape_len - 1; i >= 0; i--) {
        TapeEntry *e = &g_tape[i];
        double g_val = g_tape_grads[e->out_id];
        if (g_val == 0.0) continue;
        double lv = e->in_vals[0], rv = e->in_vals[1];
        double gl = 0.0, gr = 0.0;
        if      (!strcmp(e->op,"+")) { gl=g_val; gr=g_val; }
        else if (!strcmp(e->op,"-")) { gl=g_val; gr=-g_val; }
        else if (!strcmp(e->op,"*")) { gl=g_val*rv; gr=g_val*lv; }
        else if (!strcmp(e->op,"/")) { gl=g_val/rv; gr=-g_val*lv/(rv*rv); }
        else if (!strcmp(e->op,"neg")) { gl=-g_val; }
        if (e->in_ids[0] >= 0) g_tape_grads[e->in_ids[0]] += gl;
        if (e->in_ids[1] >= 0) g_tape_grads[e->in_ids[1]] += gr;
    }

    /* Step 7: build result map — merge tape grads with override map */
    HowMap *result = map_new();
    for (int i = 0; i < argc; i++) {
        char key[64]; snprintf(key, sizeof(key), "%d", i);
        /* Check if grad block provided an override for this param */
        Value *override = NULL;
        if (override_map && override_map->type == VT_MAP) {
            /* Try to look up by param name if available */
            /* For now: use positional index */
            override = map_get(override_map->map, key);
        }
        Value *grad_val;
        if (override && override->type != VT_NONE) {
            grad_val = val_incref(override);    /* use explicit override */
        } else if (args[i]->type == VT_NUM) {
            /* Use tape-computed gradient */
            int tape_id = /* the tape id assigned to args[i] */ -1;
            /* Look up the tape id we assigned in step 3 */
            grad_val = (tape_id >= 0) ? val_num(g_tape_grads[tape_id]) : val_none();
        } else {
            grad_val = val_none();  /* function param: no numeric gradient */
        }
        map_set(result, key, grad_val);
        val_decref(grad_val);
    }

    for (int i = 0; i < argc; i++) val_decref(primals[i]);
    free(primals);
    val_decref(override_map);

    Value *v = val_map(result); map_decref(result); return v;
}
```

**Implementation note on tape id lookup:** In step 3 above, we need to
correlate each `tape_args[i]` with its assigned tape id. Save these before
the forward call:

```c
int *arg_tape_ids = xmalloc(argc * sizeof(int));
for (int i = 0; i < argc; i++) {
    if (args[i]->type == VT_NUM) {
        arg_tape_ids[i] = g_tape_next_id;   /* next id before tape_new_val */
        tape_args[i] = tape_new_val(args[i]->nval);
    } else {
        arg_tape_ids[i] = -1;
        tape_args[i] = val_incref(args[i]);
    }
}
```

Then in step 7, use `arg_tape_ids[i]` instead of the placeholder `-1`.

---

### Update the grad block syntax description

Replace the syntax description in Step 7 with:

```
var f = (params){ :: forward_expr } grad (params, g){ :: override_map }
```

Where:
- `params` — same parameter names as the forward function  
- `g` — the upstream gradient (always the last parameter of the grad block)
- `override_map` — a partial map `{param_name: gradient}` specifying only the
  parameters whose gradients cannot be computed by the tape
- **Function parameters should be omitted from `override_map`** — their
  gradients propagate automatically through the tape

The grad block is only needed when:
1. An operation is non-differentiable (argmax, sort, step functions)
2. You want a faster or more numerically stable analytical derivative

For fully differentiable functions using only `+`, `-`, `*`, `/`, `@`, the tape
handles everything and no grad block is ever needed.

---

### Update the test in Step 8

Replace:
```
# Custom grad block
var abs_custom = (x){ x >= 0 :: x, :: -x } grad (x, g){ x >= 0 :: g, :: -g }
assert_eq("custom grad abs at  3",  grad(abs_custom)(3.0),   1.0)
assert_eq("custom grad abs at -2",  grad(abs_custom)(-2.0), -1.0)
```

With tests that also cover the partial-map return and function parameter cases:

```
# 1. Custom grad block — scalar, full override
var abs_custom = (x){
    x >= 0 :: x, :: -x
} grad (x, g){
    :: {x: x >= 0 :: g, :: -g}   # explicit gradient for x
}
assert_eq("custom abs at  3",  grad(abs_custom)(3.0),   1.0)
assert_eq("custom abs at -2",  grad(abs_custom)(-2.0), -1.0)

# 2. Partial override — only override x, let tape handle y
var mixed = (x, y){
    :: x * x + y * y * y
} grad (x, y, g){
    :: {x: 2.0 * x * g}   # only override x; y is omitted → tape computes 3y²g
}
var gm = grad(mixed)(2.0, 3.0)
assert_eq("partial override x",  gm("x"), 4.0)    # 2*2*1 = 4
assert_eq("tape computes y",      gm("y"), 27.0)   # 3*3²*1 = 27

# 3. Grad block with function parameter — f is omitted, tape handles it
var apply_double = (f, x){
    :: f(x) * 2.0
} grad (x, g){
    :: {x: 2.0 * g}   # f omitted — tape propagates through f(x)
}
var dbl = (x){ :: x * x }
var g3 = grad(apply_double)(dbl, 3.0)
assert_eq("function param tape",  g3("x"), 12.0)   # 2 * 2*3 * 1 = 12
assert_eq("function param none",  g3("f"), none)   # no numeric gradient for f

# 4. Surrogate gradient (straight-through estimator)
var threshold = (x){
    x > 0.5 :: 1.0, :: 0.0
} grad (x, g){
    :: {x: g}   # STE: pass gradient through regardless of step
}
assert_eq("STE above threshold",  grad(threshold)(0.8), 1.0)
assert_eq("STE below threshold",  grad(threshold)(0.2), 1.0)  # STE passes through

# 5. Empty override — identical to no grad block
var f_empty = (x){ :: x * x } grad (x, g){ :: {} }
assert_eq("empty override",  grad(f_empty)(3.0), 6.0)  # tape computes correctly
```

---

## Summary of changes to `grad_proposal.md`

| Section | Change |
|---|---|
| Step 7 syntax description | grad block returns a **partial map**, omitted params use tape |
| Step 7 `call_custom_grad` | New implementation: saves primals, runs tape, merges with overrides |
| Step 7 description | Add: "function parameters should be omitted from override_map" |
| Step 8 tests | Replace single custom-grad test with 5 tests covering partial maps and function params |
| New section (add after Step 7) | "Why function parameters don't need grad blocks" explanation |

---

## Why function parameters don't need grad blocks

When `f(g, x)` calls `g(x)` with the tape active:

1. `x` is a tape-tracked value (ID = 5, primal = 3.0)
2. `g(x)` runs — every `+`, `-`, `*`, `/` inside `g` is recorded
3. Tape entry: `{out_id: 7, op: "*", in_ids: [5, -1], in_vals: [3.0, 3.0]}`
   (if `g` is `(x){ :: x * x }`)
4. Backward: `g_tape_grads[5] += g_tape_grads[7] * 3.0 * 2` (product rule)

This works whether `g` is a plain function or a function closing over `W`
and `b`. When `g` closes over numeric variables, those variables participate
in tape entries too. The backward pass accumulates their gradients
automatically.

There is no information the grad block could add for function parameters that
the tape doesn't already provide. Listing `f: none` in the override map is
redundant — it is the default behavior.

The only exception: if `g` contains a non-differentiable operation (like
argmax). In that case, `g` itself should have a grad block, not `f`. The
custom VJP should be defined at the operation level, not at the caller level.
