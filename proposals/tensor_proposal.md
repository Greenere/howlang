# Howlang — Tensor Proposal

## Context and motivation

Howlang has an MLP implementation (`mlp.how`) that runs correctly but takes
~2.3 seconds for 1000 training epochs on a trivial XOR problem. The gap vs C
is ~23,000×. The bottleneck is not algorithmic — it's that every vector
operation allocates a new list, boxes each float64 into a `Value` struct, and
immediately discards the result. A 4-element vector operation creates ~5
separate heap allocations that live for microseconds.

The fix: a `VT_TENSOR` value type with flat `double[]` storage, operator
overloading so existing `+`, `-`, `*` dispatch to C-level loops, and a new
`@` operator for matrix multiplication.

**What does NOT change:** `VT_LIST` semantics are completely preserved. `list +
list` still concatenates. `push()` still works. No existing code breaks.
The quicksort `sorted(small) + {pivot} + sorted(large)`, the BST traversal
`traverse(left) + {val} + traverse(right)`, all of it — untouched.

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

## What tensors look like from the user's perspective

```
# Create tensors — one wrapper around existing list-literal syntax:
var v = tensor({1.0, 2.0, 3.0, 4.0})           # 1D, shape {4}
var W = tensor({{1.0, 0.0}, {0.0, 1.0}})        # 2D, shape {2,2}
var W = tensor({2,2}, {1.0, 0.0, 0.0, 1.0})     # same, from flat list

# Indexing — identical syntax to lists:
v(1)          # 2.0  (scalar)
W(0)          # tensor shape {2} — row view, shares memory with W
W(0)(1)       # 0.0  (scalar via chained calls)
W(0)(1) = 9   # mutates W through the row view

# Arithmetic — element-wise for tensors:
v + v         # {2.0, 4.0, 6.0, 8.0}  (element-wise, NOT concatenation)
v * v         # {1.0, 4.0, 9.0, 16.0}
v * 2.0       # {2.0, 4.0, 6.0, 8.0}  (scalar broadcast)
v - tensor({0.5, 0.5, 0.5, 0.5})  # element-wise sub

# Matrix multiply — new @ operator:
W @ v         # shape {2}  — matrix-vector product
A @ B         # shape {m,n} — matrix-matrix product for A:{m,k}, B:{k,n}
T(W) @ v      # transpose W then multiply — replaces matvec_T

# Augmented assignment in-place:
W -= 0.05 * outer(dz, x)   # in-place update, avoids extra allocation

# Builtins:
shape(v)      # {4}
shape(W)      # {2,2}
T(W)          # transpose — swaps last two dimensions
outer(a, b)   # outer product: shape {len(a), len(b)}
sum(v)        # scalar sum of all elements
zeros({3,4})  # 3×4 tensor of zeros
ones({4})     # length-4 tensor of ones
eye(3)        # 3×3 identity matrix
```

---

## The MLP with tensor support

The migration requires wrapping 6 weight declarations in `tensor()` and
replacing the helper functions with operators:

```
# Before (list-based):
var W1 = {{ 1,  1},{ 1,  1},{ 1, -1},{-1,  1}}
var b1 = {-0.5, -1.5, 0, 0}

# After:
var W1 = tensor({{ 1.0,  1.0},{ 1.0,  1.0},{ 1.0, -1.0},{-1.0,  1.0}})
var b1 = tensor({-0.5, -1.5, 0.0, 0.0})
```

```
# Layer.forward — before:
forward: (x){
    z = vadd(matvec(W, x), b),
    h = act(z),
    :: h
}

# Layer.forward — after:
forward: (x){
    z = W @ x + b,
    h = act(z),
    :: h
}
```

```
# Layer.backward — before:
backward: (grad_h, x_in, lr){
    var grad_z = vmul(grad_h, act_grad(z)),
    var grad_x = matvec_T(W, grad_z),
    W = matsub(W, matscale(outer(grad_z, x_in), lr)),
    b = vsub(b, vscale(grad_z, lr)),
    :: grad_x
}

# Layer.backward — after:
backward: (grad_h, x_in, lr){
    var grad_z = grad_h * act_grad(z),
    var grad_x = T(W) @ grad_z,
    W -= lr * outer(grad_z, x_in),
    b -= lr * grad_z,
    :: grad_x
}
```

```
# Activation functions — before (8-line loops):
var relu = (v){
    var out = list()
    (i=0:len(v)){ push(out, max(0, v(i))) }()
    :: out
}

# After (max() vectorized over tensors):
var relu = (v){ :: max(0, v) }
```

**Functions that disappear entirely:** `vadd`, `vsub`, `vmul`, `vscale`, `dot`,
`matvec`, `matvec_T`, `matsub`, `matscale`, `relu_grad`, `sigmoid_grad` — all
replaced by operators. `outer` stays as a named builtin.

---

## Implementation

### Step 1: `HowTensor` struct and `VT_TENSOR` value type

Add to the `VT` enum (alongside `VT_LIST`, `VT_MAP`, etc.):
```c
VT_TENSOR,
```

Define the tensor struct (add near `HowList`, `HowMap`):
```c
typedef struct HowTensor HowTensor;

struct HowTensor {
    double      *data;      /* flat row-major float64 array */
    int         *shape;     /* dimension sizes, e.g. {4, 2} */
    int          ndim;      /* number of dimensions */
    int          nelem;     /* total number of elements (product of shape) */
    int         *strides;   /* stride per dimension in elements (row-major) */
    HowTensor   *base;      /* non-NULL if this is a view into another tensor */
    int          refcount;
    int          gc_mark;
    struct HowTensor *gc_next;
};

static HowTensor *g_all_tensors = NULL;
```

Constructor:
```c
static HowTensor *tensor_new(int ndim, int *shape) {
    HowTensor *t = xmalloc(sizeof(*t));
    t->ndim   = ndim;
    t->shape  = xmalloc(ndim * sizeof(int));
    t->strides = xmalloc(ndim * sizeof(int));
    memcpy(t->shape, shape, ndim * sizeof(int));
    t->nelem = 1;
    for (int i = 0; i < ndim; i++) t->nelem *= shape[i];
    /* Row-major strides */
    t->strides[ndim-1] = 1;
    for (int i = ndim-2; i >= 0; i--)
        t->strides[i] = t->strides[i+1] * shape[i+1];
    t->data   = xmalloc(t->nelem * sizeof(double));
    t->base   = NULL;
    t->refcount = 1;
    t->gc_mark  = 0;
    t->gc_next  = g_all_tensors;
    g_all_tensors = t;
    g_gc_allocations++;
    return t;
}

static Value *val_tensor(HowTensor *t) {
    Value *v = val_new(VT_TENSOR);
    v->tensor = t;
    t->refcount++;
    return v;
}
```

Add `HowTensor *tensor` to the `Value` union.

Add `tensor_decref` and GC support:
```c
static void tensor_decref(HowTensor *t) {
    if (!t) return;
    t->refcount--;
}
```

In `val_decref`, add:
```c
case VT_TENSOR: tensor_decref(v->tensor); break;
```

In `val_repr`, add:
```c
case VT_TENSOR: {
    Buf b = {0};
    buf_append(&b, "[");
    for (int i = 0; i < t->nelem; i++) {
        if (i) buf_append(&b, ", ");
        char tmp[32];
        double d = t->data[i];
        if (d == (long long)d) snprintf(tmp,sizeof(tmp),"%lld",(long long)d);
        else snprintf(tmp,sizeof(tmp),"%g",d);
        buf_append(&b, tmp);
    }
    buf_append(&b, "]");
    /* Prefix shape for 2D+ tensors */
    if (t->ndim > 1) { /* optionally: prepend shape */ }
    return buf_done(&b);
}
```

GC mark and sweep — follow the same pattern as `HowList`:
```c
static void gc_mark_tensor(HowTensor *t) {
    if (!t || t->gc_mark) return;
    t->gc_mark = 1;
    if (t->base) gc_mark_tensor(t->base);  /* keep base alive if view exists */
}

static void gc_sweep_tensors(void) {
    HowTensor **pp = &g_all_tensors;
    while (*pp) {
        HowTensor *t = *pp;
        if (t->gc_mark) { t->gc_mark = 0; pp = &t->gc_next; continue; }
        *pp = t->gc_next;
        if (!t->base) { free(t->data); }  /* don't free view's data (owned by base) */
        free(t->shape);
        free(t->strides);
        free(t);
    }
}
```

Call `gc_sweep_tensors()` from `gc_collect()` alongside the other sweep calls.

In `gc_mark_value`, add:
```c
case VT_TENSOR: gc_mark_tensor(v->tensor); break;
```

Add `type_fn` case:
```c
case VT_TENSOR: return val_str("tensor");
```

---

### Step 2: Row view — `t(i)` returns a view

In `eval_call_val`, add the tensor call case **before** the list call case:

```c
/* Tensor call: t(i) → row view (rank n-1 tensor) or scalar (rank 0) */
if (callee->type == VT_TENSOR) {
    HowTensor *t = callee->tensor;
    if (argc != 1) die_at(line, 0, "tensor call requires exactly 1 argument");
    if (args[0]->type != VT_NUM) die_at(line, 0, "tensor index must be a number");
    int i = (int)args[0]->nval;
    if (i < 0 || i >= t->shape[0])
        die_at(line, 0, "tensor index %d out of range (size %d)", i, t->shape[0]);

    if (t->ndim == 1) {
        /* 1D tensor: return scalar */
        return val_num(t->data[i * t->strides[0]]);
    }

    /* nD tensor: return (n-1)D view */
    HowTensor *view = xmalloc(sizeof(*view));
    view->ndim    = t->ndim - 1;
    view->shape   = xmalloc(view->ndim * sizeof(int));
    view->strides = xmalloc(view->ndim * sizeof(int));
    for (int d = 0; d < view->ndim; d++) {
        view->shape[d]   = t->shape[d+1];
        view->strides[d] = t->strides[d+1];
    }
    view->nelem     = 1;
    for (int d = 0; d < view->ndim; d++) view->nelem *= view->shape[d];
    view->data      = t->data + i * t->strides[0];  /* pointer into parent */
    view->base      = t; t->refcount++;              /* hold parent alive */
    view->refcount  = 1;
    view->gc_mark   = 0;
    view->gc_next   = g_all_tensors;
    g_all_tensors   = view;
    g_gc_allocations++;

    return val_tensor(view);
}
```

**Write through a view** — extend the `m(k) = v` assignment path in `N_ASSIGN`
to handle `VT_TENSOR`:

```c
} else if (obj->type == VT_TENSOR) {
    HowTensor *t = obj->tensor;
    if (t->ndim != 1)
        die_at(node->line, 0, "use t(i)(j) = v to write into a 2D tensor row");
    int idx = (int)key->nval; val_decref(key);
    if (idx < 0 || idx >= t->shape[0])
        die_at(node->line, 0, "tensor index %d out of range", idx);
    if (!strcmp(op, "=")) {
        t->data[idx * t->strides[0]] = val->nval;
    } else {
        double old = t->data[idx * t->strides[0]];
        double newv;
        if      (!strcmp(op,"+=")) newv = old + val->nval;
        else if (!strcmp(op,"-=")) newv = old - val->nval;
        else if (!strcmp(op,"*=")) newv = old * val->nval;
        else if (!strcmp(op,"/=")) newv = old / val->nval;
        else die_at(node->line, 0, "unknown augop on tensor element");
        t->data[idx * t->strides[0]] = newv;
    }
```

---

### Step 3: Operator overloading in `N_BINOP`

Add a tensor arithmetic helper before `eval`:

```c
/* Check shapes match for element-wise ops */
static void tensor_check_shapes(HowTensor *a, HowTensor *b, int line, const char *op) {
    if (a->ndim != b->ndim || a->nelem != b->nelem) {
        /* Build shape strings for the error message */
        char sa[64] = "{", sb[64] = "{";
        for (int i = 0; i < a->ndim; i++) {
            char tmp[16]; snprintf(tmp,sizeof(tmp),"%d%s",a->shape[i],i<a->ndim-1?",":"");
            strncat(sa,tmp,sizeof(sa)-strlen(sa)-1);
        }
        strncat(sa,"}",sizeof(sa)-strlen(sa)-1);
        for (int i = 0; i < b->ndim; i++) {
            char tmp[16]; snprintf(tmp,sizeof(tmp),"%d%s",b->shape[i],i<b->ndim-1?",":"");
            strncat(sb,tmp,sizeof(sb)-strlen(sb)-1);
        }
        strncat(sb,"}",sizeof(sb)-strlen(sb)-1);
        die_at(line, 0, "tensor shapes %s and %s incompatible for '%s'", sa, sb, op);
    }
}

/* Element-wise binary op on two tensors */
static Value *tensor_elemwise(HowTensor *a, HowTensor *b, const char *op, int line) {
    tensor_check_shapes(a, b, line, op);
    HowTensor *out = tensor_new(a->ndim, a->shape);
    for (int i = 0; i < a->nelem; i++) {
        double av = a->data[i], bv = b->data[i];
        if      (!strcmp(op,"+")) out->data[i] = av + bv;
        else if (!strcmp(op,"-")) out->data[i] = av - bv;
        else if (!strcmp(op,"*")) out->data[i] = av * bv;
        else if (!strcmp(op,"/")) {
            if (bv == 0) die_at(line, 0, "tensor division by zero at index %d", i);
            out->data[i] = av / bv;
        }
        else die_at(line, 0, "unsupported element-wise op '%s'", op);
    }
    Value *v = val_tensor(out); tensor_decref(out); return v;
}

/* Tensor scaled by a scalar */
static Value *tensor_scale(HowTensor *t, double s, int line) {
    HowTensor *out = tensor_new(t->ndim, t->shape);
    for (int i = 0; i < t->nelem; i++) out->data[i] = t->data[i] * s;
    Value *v = val_tensor(out); tensor_decref(out); return v;
}
```

In `eval N_BINOP`, **before** the existing `if (!strcmp(op,"+"))` block, add:

```c
/* ── Tensor arithmetic ──────────────────────────────────────────────────── */
if (l->type == VT_TENSOR || r->type == VT_TENSOR) {
    /* tensor @ tensor — matrix multiply (handled separately) */
    if (!strcmp(op, "@")) {
        if (l->type!=VT_TENSOR || r->type!=VT_TENSOR)
            die_at(node->line, 0, "@ requires two tensors");
        res = tensor_matmul(l->tensor, r->tensor, node->line);
        GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
        val_decref(l); val_decref(r);
        return res;
    }
    /* tensor OP tensor — element-wise */
    if (l->type==VT_TENSOR && r->type==VT_TENSOR) {
        if (!strcmp(op,"+")||!strcmp(op,"-")||!strcmp(op,"*")||!strcmp(op,"/")) {
            res = tensor_elemwise(l->tensor, r->tensor, op, node->line);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            val_decref(l); val_decref(r);
            return res;
        }
    }
    /* tensor * scalar  or  scalar * tensor */
    if (!strcmp(op,"*")) {
        if (l->type==VT_TENSOR && r->type==VT_NUM) {
            res = tensor_scale(l->tensor, r->nval, node->line);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            val_decref(l); val_decref(r);
            return res;
        }
        if (l->type==VT_NUM && r->type==VT_TENSOR) {
            res = tensor_scale(r->tensor, l->nval, node->line);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            val_decref(l); val_decref(r);
            return res;
        }
    }
    /* tensor + scalar  or  scalar + tensor (broadcast add) */
    if (!strcmp(op,"+") || !strcmp(op,"-")) {
        double s = 0;
        HowTensor *t = NULL;
        int sign = !strcmp(op,"-") ? -1 : 1;
        if (l->type==VT_TENSOR && r->type==VT_NUM) { t=l->tensor; s=r->nval*sign; }
        if (l->type==VT_NUM && r->type==VT_TENSOR) { t=r->tensor; s=l->nval; sign=1; }
        if (t) {
            HowTensor *out = tensor_new(t->ndim, t->shape);
            for (int i=0; i<t->nelem; i++)
                out->data[i] = (l->type==VT_TENSOR) ? t->data[i]+s : s+t->data[i]*sign;
            res = val_tensor(out); tensor_decref(out);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            val_decref(l); val_decref(r);
            return res;
        }
    }
    die_at(node->line, 0, "unsupported tensor operation '%s'", op);
}
/* ── End tensor arithmetic ──────────────────────────────────────────────── */
```

Also add tensor support to `apply_augop` (for `W -= lr * grad`):

```c
/* In apply_augop, before the existing += check: */
if (old->type == VT_TENSOR) {
    if (!strcmp(op,"+=") || !strcmp(op,"-=") || !strcmp(op,"*=") || !strcmp(op,"/=")) {
        const char *base_op = !strcmp(op,"+=") ? "+" :
                              !strcmp(op,"-=") ? "-" :
                              !strcmp(op,"*=") ? "*" : "/";
        Value *old_v = old; val->refcount++;  /* temp ref */
        Value *res;
        if (val->type == VT_TENSOR)
            res = tensor_elemwise(old->tensor, val->tensor, base_op, line);
        else if (val->type == VT_NUM) {
            if (!strcmp(base_op,"*") || !strcmp(base_op,"/"))
                res = tensor_scale(old->tensor,
                      !strcmp(base_op,"*") ? val->nval : 1.0/val->nval, line);
            else {
                /* += or -= scalar: broadcast */
                HowTensor *out = tensor_new(old->tensor->ndim, old->tensor->shape);
                double s = (!strcmp(base_op,"+")) ? val->nval : -val->nval;
                for (int i=0; i<out->nelem; i++) out->data[i] = old->tensor->data[i] + s;
                res = val_tensor(out); tensor_decref(out);
            }
        } else die_at(line, 0, "incompatible types for tensor %s", op);
        val_decref(val); GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
        return res;
    }
}
```

---

### Step 4: `@` operator — matrix multiply

**Lexer:** Add `TT_AT` to the `TT` enum. In the `switch(c)` block:
```c
case '@': t.type = TT_AT; break;
```

**Parser:** In `parse_mul`, add `@` alongside `*`, `/`, `%`:
```c
if      (p_check(p,TT_STAR))    op="*";
else if (p_check(p,TT_SLASH))   op="/";
else if (p_check(p,TT_PERCENT)) op="%";
else if (p_check(p,TT_AT))      op="@";   /* ← add this line */
else break;
```

**Matmul implementation:**
```c
static Value *tensor_matmul(HowTensor *a, HowTensor *b, int line) {
    /* Supported shapes:
       (m,k) @ (k,n) → (m,n)
       (m,k) @ (k)   → (m)    matrix-vector
       (k)   @ (k)   → scalar  dot product    */
    if (a->ndim == 1 && b->ndim == 1) {
        /* Dot product */
        if (a->nelem != b->nelem)
            die_at(line, 0, "dot product: shapes {%d} and {%d} incompatible",
                   a->nelem, b->nelem);
        double s = 0;
        for (int i = 0; i < a->nelem; i++) s += a->data[i] * b->data[i];
        return val_num(s);
    }
    if (a->ndim == 2 && b->ndim == 1) {
        /* Matrix-vector: (m,k) @ (k) → (m) */
        int m = a->shape[0], k = a->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d} incompatible", m, k, b->shape[0]);
        int out_shape[] = {m};
        HowTensor *out = tensor_new(1, out_shape);
        for (int i = 0; i < m; i++) {
            double s = 0;
            for (int j = 0; j < k; j++) s += a->data[i*k + j] * b->data[j];
            out->data[i] = s;
        }
        Value *v = val_tensor(out); tensor_decref(out); return v;
    }
    if (a->ndim == 2 && b->ndim == 2) {
        /* Matrix-matrix: (m,k) @ (k,n) → (m,n) */
        int m = a->shape[0], k = a->shape[1], n = b->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d,%d} incompatible",
                   m, k, b->shape[0], n);
        int out_shape[] = {m, n};
        HowTensor *out = tensor_new(2, out_shape);
        for (int i = 0; i < m; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int p = 0; p < k; p++) s += a->data[i*k+p] * b->data[p*n+j];
                out->data[i*n+j] = s;
            }
        Value *v = val_tensor(out); tensor_decref(out); return v;
    }
    die_at(line, 0, "matmul: unsupported tensor dimensions %dD @ %dD", a->ndim, b->ndim);
    return val_none();
}
```

---

### Step 5: Builtins

Register all in `setup_globals()`:
```c
REG("tensor",  tensor_fn);
REG("shape",   shape_fn);
REG("T",       transpose_fn);
REG("outer",   outer_fn);
REG("zeros",   zeros_fn);
REG("ones",    ones_fn);
REG("eye",     eye_fn);
```

Also extend `sum()`, `abs()`, `max()`, `min()`, `sqrt()`, `len()` to handle tensors.

**`tensor(data)` or `tensor(shape, data)`:**
```c
BUILTIN(tensor_fn) {
    if (argc == 0) die("tensor() requires at least 1 argument");

    /* tensor(flat_list_or_nested_list) — infer shape from nesting */
    if (argc == 1) {
        Value *arg = ARG(0);
        if (arg->type == VT_LIST) {
            /* Check if it's a list of lists (2D) or list of numbers (1D) */
            HowList *outer = arg->list;
            if (outer->len == 0) {
                int shape[] = {0};
                HowTensor *t = tensor_new(1, shape);
                Value *v = val_tensor(t); tensor_decref(t); return v;
            }
            if (outer->items[0]->type == VT_LIST) {
                /* 2D: list of lists */
                int rows = outer->len;
                int cols = outer->items[0]->list->len;
                int shape[] = {rows, cols};
                HowTensor *t = tensor_new(2, shape);
                for (int i = 0; i < rows; i++) {
                    HowList *row = outer->items[i]->list;
                    if (row->len != cols)
                        die("tensor(): all rows must have the same length");
                    for (int j = 0; j < cols; j++) {
                        if (row->items[j]->type != VT_NUM)
                            die("tensor(): all elements must be numbers");
                        t->data[i*cols + j] = row->items[j]->nval;
                    }
                }
                Value *v = val_tensor(t); tensor_decref(t); return v;
            }
            /* 1D: list of numbers */
            int shape[] = {outer->len};
            HowTensor *t = tensor_new(1, shape);
            for (int i = 0; i < outer->len; i++) {
                if (outer->items[i]->type != VT_NUM)
                    die("tensor(): all elements must be numbers");
                t->data[i] = outer->items[i]->nval;
            }
            Value *v = val_tensor(t); tensor_decref(t); return v;
        }
        die("tensor() requires a list argument");
    }

    /* tensor(shape_list, flat_data_list) */
    if (argc == 2) {
        if (ARG(0)->type != VT_LIST) die("tensor(): first arg must be shape list");
        if (ARG(1)->type != VT_LIST) die("tensor(): second arg must be data list");
        HowList *shape_list = ARG(0)->list;
        HowList *data_list  = ARG(1)->list;
        int ndim = shape_list->len;
        int *shape = xmalloc(ndim * sizeof(int));
        for (int i = 0; i < ndim; i++) shape[i] = (int)shape_list->items[i]->nval;
        HowTensor *t = tensor_new(ndim, shape);
        free(shape);
        if (data_list->len != t->nelem)
            die("tensor(): data length %d doesn't match shape (expected %d)",
                data_list->len, t->nelem);
        for (int i = 0; i < t->nelem; i++) {
            if (data_list->items[i]->type != VT_NUM)
                die("tensor(): all data elements must be numbers");
            t->data[i] = data_list->items[i]->nval;
        }
        Value *v = val_tensor(t); tensor_decref(t); return v;
    }
    die("tensor() takes 1 or 2 arguments");
    return val_none();
}
```

**`shape(t)`:**
```c
BUILTIN(shape_fn) {
    NEED(1);
    if (ARG(0)->type != VT_TENSOR) die("shape() requires a tensor");
    HowTensor *t = ARG(0)->tensor;
    HowList *l = list_new();
    for (int i = 0; i < t->ndim; i++) {
        Value *v = val_num(t->shape[i]);
        list_push(l, v); val_decref(v);
    }
    Value *v = val_list(l); list_decref(l); return v;
}
```

**`T(t)` — transpose:**
```c
BUILTIN(transpose_fn) {
    NEED(1);
    if (ARG(0)->type != VT_TENSOR) die("T() requires a tensor");
    HowTensor *t = ARG(0)->tensor;
    if (t->ndim < 2) die("T() requires a tensor with at least 2 dimensions");
    /* For 2D: swap shape[0] and shape[1], copy with transposed indexing */
    int m = t->shape[0], n = t->shape[1];
    int out_shape[] = {n, m};
    HowTensor *out = tensor_new(2, out_shape);
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++)
            out->data[j*m + i] = t->data[i*n + j];
    Value *v = val_tensor(out); tensor_decref(out); return v;
}
```

**`outer(a, b)` — outer product:**
```c
BUILTIN(outer_fn) {
    NEED(2);
    if (ARG(0)->type != VT_TENSOR || ARG(1)->type != VT_TENSOR)
        die("outer() requires two tensors");
    HowTensor *a = ARG(0)->tensor, *b = ARG(1)->tensor;
    if (a->ndim != 1 || b->ndim != 1) die("outer() requires 1D tensors");
    int shape[] = {a->nelem, b->nelem};
    HowTensor *out = tensor_new(2, shape);
    for (int i = 0; i < a->nelem; i++)
        for (int j = 0; j < b->nelem; j++)
            out->data[i * b->nelem + j] = a->data[i] * b->data[j];
    Value *v = val_tensor(out); tensor_decref(out); return v;
}
```

**`zeros(shape)`, `ones(shape)`, `eye(n)`:**
```c
BUILTIN(zeros_fn) {
    NEED(1);
    if (ARG(0)->type != VT_LIST) die("zeros() requires a shape list");
    HowList *sl = ARG(0)->list;
    int ndim = sl->len;
    int *shape = xmalloc(ndim * sizeof(int));
    for (int i = 0; i < ndim; i++) shape[i] = (int)sl->items[i]->nval;
    HowTensor *t = tensor_new(ndim, shape);
    free(shape);
    memset(t->data, 0, t->nelem * sizeof(double));
    Value *v = val_tensor(t); tensor_decref(t); return v;
}

BUILTIN(ones_fn) {
    NEED(1);
    if (ARG(0)->type != VT_LIST) die("ones() requires a shape list");
    HowList *sl = ARG(0)->list;
    int ndim = sl->len;
    int *shape = xmalloc(ndim * sizeof(int));
    for (int i = 0; i < ndim; i++) shape[i] = (int)sl->items[i]->nval;
    HowTensor *t = tensor_new(ndim, shape);
    free(shape);
    for (int i = 0; i < t->nelem; i++) t->data[i] = 1.0;
    free(shape);
    Value *v = val_tensor(t); tensor_decref(t); return v;
}

BUILTIN(eye_fn) {
    NEED(1);
    int n = (int)ARG(0)->nval;
    int shape[] = {n, n};
    HowTensor *t = tensor_new(2, shape);
    memset(t->data, 0, t->nelem * sizeof(double));
    for (int i = 0; i < n; i++) t->data[i*n + i] = 1.0;
    Value *v = val_tensor(t); tensor_decref(t); return v;
}
```

**Extend `len()` for tensors** (returns size of first dimension):
```c
/* In builtin_len_fn, add: */
if (v->type == VT_TENSOR) return val_num(v->tensor->shape[0]);
```

**Extend `sum()` for tensors:**
```c
/* In builtin... or add a new sum_fn: */
BUILTIN(sum_fn) {
    NEED(1);
    if (ARG(0)->type == VT_TENSOR) {
        double s = 0;
        HowTensor *t = ARG(0)->tensor;
        for (int i = 0; i < t->nelem; i++) s += t->data[i];
        return val_num(s);
    }
    /* Fall through to existing list sum logic */
    die("sum() requires a tensor or list");
    return val_none();
}
```

**Extend `max()` to broadcast scalar over tensor** (for `relu = max(0, v)`):
```c
/* In builtin_max_fn, add before existing logic: */
if (argc == 2 && ARG(0)->type == VT_NUM && ARG(1)->type == VT_TENSOR) {
    double threshold = ARG(0)->nval;
    HowTensor *t = ARG(1)->tensor;
    HowTensor *out = tensor_new(t->ndim, t->shape);
    for (int i = 0; i < t->nelem; i++)
        out->data[i] = t->data[i] > threshold ? t->data[i] : threshold;
    Value *v = val_tensor(out); tensor_decref(out); return v;
}
```

**Extend `abs()` for tensors:**
```c
/* In builtin_abs_fn: */
if (ARG(0)->type == VT_TENSOR) {
    HowTensor *t = ARG(0)->tensor;
    HowTensor *out = tensor_new(t->ndim, t->shape);
    for (int i = 0; i < t->nelem; i++) out->data[i] = fabs(t->data[i]);
    Value *v = val_tensor(out); tensor_decref(out); return v;
}
```

---

### Step 6: Verification tests

Add to `test_all.how` or a new `test_tensor.how`:

```
# Construction
var v = tensor({1.0, 2.0, 3.0})
var W = tensor({{1.0, 0.0}, {0.0, 1.0}})
assert_eq("tensor 1D index",   v(1),       2.0)
assert_eq("tensor 2D index",   W(0)(0),    1.0)
assert_eq("tensor 2D index",   W(1)(1),    1.0)
assert_eq("tensor shape len",  len(v),     3)
assert_eq("tensor shape",      shape(W)(0), 2)

# Element-wise ops
var a = tensor({1.0, 2.0, 3.0})
var b = tensor({4.0, 5.0, 6.0})
var c = a + b
assert_eq("tensor add 0", c(0), 5.0)
assert_eq("tensor add 1", c(1), 7.0)
assert_eq("tensor add 2", c(2), 9.0)

var d = a * 2.0
assert_eq("tensor scale 0", d(0), 2.0)
assert_eq("tensor scale 1", d(1), 4.0)

# Matmul
var I = eye(3)
var x = tensor({1.0, 2.0, 3.0})
var y = I @ x
assert_eq("matmul identity 0", y(0), 1.0)
assert_eq("matmul identity 1", y(1), 2.0)
assert_eq("matmul identity 2", y(2), 3.0)

# Mutation through view
var M = tensor({{1.0, 2.0}, {3.0, 4.0}})
M(0)(1) = 99.0
assert_eq("view mutation", M(0)(1), 99.0)
assert_eq("view mutation other unchanged", M(1)(0), 3.0)

# Transpose
var A = tensor({{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}})
var At = T(A)
assert_eq("transpose shape rows", shape(At)(0), 3)
assert_eq("transpose shape cols", shape(At)(1), 2)
assert_eq("transpose element",    At(0)(1), 4.0)

# Outer product
var p = tensor({1.0, 2.0})
var q = tensor({3.0, 4.0, 5.0})
var pq = outer(p, q)
assert_eq("outer shape 0", shape(pq)(0), 2)
assert_eq("outer shape 1", shape(pq)(1), 3)
assert_eq("outer element", pq(1)(2), 10.0)   # 2 * 5

# Relu via max
var z = tensor({-1.0, 0.5, -0.3, 2.0})
var h = max(0, z)
assert_eq("relu neg",  h(0), 0.0)
assert_eq("relu pos",  h(1), 0.5)
assert_eq("relu zero", h(2), 0.0)
assert_eq("relu big",  h(3), 2.0)

# List semantics UNCHANGED
var lst = {1, 2, 3}
var lst2 = {4, 5, 6}
var cat = lst + lst2
assert_eq("list concat still works", len(cat), 6)
assert_eq("list concat element",     cat(3),   4)
```

---

## What does NOT change

- `VT_LIST` semantics: `list + list` still concatenates, `push()` still works
- All existing `.how` files: no changes needed, all 5 test suites pass unchanged
- The call syntax: `t(i)`, `t(i)(j)`, `t(i)(j) = v` work identically to lists
- The branch model, classes, loops: completely untouched
- The `@` token does not conflict with anything: `@` is currently an unexpected
  character that raises a lex error — now it's a valid operator

## Optional: BLAS acceleration

Once the above is working correctly, replace the inner loops in `tensor_matmul`
and `tensor_elemwise` with BLAS calls for large tensors:

```c
#ifdef HAVE_BLAS
#include <cblas.h>
/* In tensor_matmul for 2D @ 2D case: */
cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
            m, n, k, 1.0, a->data, k, b->data, n, 0.0, out->data, n);
#endif
```

Compile with `-lblas` when available. Fall back to the C loops otherwise.
