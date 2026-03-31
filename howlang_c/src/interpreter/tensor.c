/*
 * tensor.c — tensor-specific operations shared across the evaluator and builtins.
 *
 * Ownership / GC stay in gc.c, tensor call syntax (t(i)) stays in call.c, and
 * this file handles the reusable math / shape / construction helpers that were
 * previously split across runtime.c and builtins.c.
 *
 * Exports (declared in interpreter/runtime_internal.h):
 *   tensor_binop(), tensor_apply_augop()
 *   tensor_build_from_args(), tensor_shape_value(), tensor_transpose_value()
 *   tensor_outer_value(), tensor_zeros_value(), tensor_ones_value()
 *   tensor_eye_value(), tensor_sum_value(), tensor_max_value(),
 *   tensor_min_value()
 */
#include "interpreter/runtime_internal.h"

/* ── Internal shape / arithmetic helpers ────────────────────────────────── */

static void tensor_shape_str(HowTensor *t, char *buf, int bufsz) {
    int pos = 0;
    pos += snprintf(buf + pos, bufsz - pos, "{");
    for (int i = 0; i < t->ndim; i++)
        pos += snprintf(buf + pos, bufsz - pos, "%d%s", t->shape[i], i < t->ndim - 1 ? "," : "");
    snprintf(buf + pos, bufsz - pos, "}");
}

static void tensor_check_shapes(HowTensor *a, HowTensor *b, int line, const char *op) {
    int ok = (a->ndim == b->ndim);
    for (int d = 0; ok && d < a->ndim; d++)
        ok = (a->shape[d] == b->shape[d]);
    if (!ok) {
        char sa[64], sb[64];
        tensor_shape_str(a, sa, sizeof(sa));
        tensor_shape_str(b, sb, sizeof(sb));
        die_at(line, 0, "tensor shapes %s and %s incompatible for '%s'", sa, sb, op);
    }
}

static Value *tensor_elemwise(HowTensor *a, HowTensor *b, const char *op, int line) {
    tensor_check_shapes(a, b, line, op);
    HowTensor *out = tensor_new(a->ndim, a->shape);
    for (int i = 0; i < a->nelem; i++) {
        double av = a->data[i], bv = b->data[i];
        if      (!strcmp(op, "+")) out->data[i] = av + bv;
        else if (!strcmp(op, "-")) out->data[i] = av - bv;
        else if (!strcmp(op, "*")) out->data[i] = av * bv;
        else if (!strcmp(op, "/")) {
            if (bv == 0) die_at(line, 0, "tensor division by zero at index %d", i);
            out->data[i] = av / bv;
        } else {
            die_at(line, 0, "unsupported element-wise tensor op '%s'", op);
        }
    }
    return val_tensor(out);
}

static Value *tensor_scale(HowTensor *t, double s, int line) {
    (void)line;
    HowTensor *out = tensor_new(t->ndim, t->shape);
    for (int i = 0; i < t->nelem; i++) out->data[i] = t->data[i] * s;
    return val_tensor(out);
}

static Value *tensor_matmul(HowTensor *a, HowTensor *b, int line) {
    if (a->ndim == 1 && b->ndim == 1) {
        if (a->shape[0] != b->shape[0])
            die_at(line, 0, "dot product: shapes {%d} and {%d} incompatible", a->shape[0], b->shape[0]);
        double s = 0;
        for (int i = 0; i < a->shape[0]; i++)
            s += a->data[i * a->strides[0]] * b->data[i * b->strides[0]];
        return val_num(s);
    }
    if (a->ndim == 2 && b->ndim == 1) {
        int m = a->shape[0], k = a->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d} incompatible", m, k, b->shape[0]);
        int out_shape[] = {m};
        HowTensor *out = tensor_new(1, out_shape);
        for (int i = 0; i < m; i++) {
            double s = 0;
            for (int j = 0; j < k; j++)
                s += a->data[i * a->strides[0] + j * a->strides[1]] * b->data[j * b->strides[0]];
            out->data[i] = s;
        }
        return val_tensor(out);
    }
    if (a->ndim == 2 && b->ndim == 2) {
        int m = a->shape[0], k = a->shape[1], n = b->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d,%d} incompatible", m, k, b->shape[0], n);
        int out_shape[] = {m, n};
        HowTensor *out = tensor_new(2, out_shape);
        for (int i = 0; i < m; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int p = 0; p < k; p++)
                    s += a->data[i * a->strides[0] + p * a->strides[1]]
                       * b->data[p * b->strides[0] + j * b->strides[1]];
                out->data[i * n + j] = s;
            }
        return val_tensor(out);
    }
    die_at(line, 0, "matmul: unsupported %dD @ %dD", a->ndim, b->ndim);
    return val_none();
}

/* ── Public tensor arithmetic helpers ───────────────────────────────────── */

Value *tensor_binop(Value *l, Value *r, const char *op, int line) {
    if (!strcmp(op, "@")) {
        if (l->type != VT_TENSOR || r->type != VT_TENSOR)
            die_at(line, 0, "@ requires two tensors");
        return tensor_matmul(l->tensor, r->tensor, line);
    }
    if (l->type == VT_TENSOR && r->type == VT_TENSOR) {
        if (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") || !strcmp(op, "/"))
            return tensor_elemwise(l->tensor, r->tensor, op, line);
        die_at(line, 0, "unsupported tensor-tensor op '%s'", op);
    }
    if (!strcmp(op, "*")) {
        if (l->type == VT_TENSOR && r->type == VT_NUM) return tensor_scale(l->tensor, r->nval, line);
        if (l->type == VT_NUM && r->type == VT_TENSOR) return tensor_scale(r->tensor, l->nval, line);
    }
    if (!strcmp(op, "/") && l->type == VT_TENSOR && r->type == VT_NUM) {
        if (r->nval == 0) die_at(line, 0, "tensor division by zero");
        return tensor_scale(l->tensor, 1.0 / r->nval, line);
    }
    if (!strcmp(op, "+") || !strcmp(op, "-")) {
        if (l->type == VT_TENSOR && r->type == VT_NUM) {
            double s = !strcmp(op, "+") ? r->nval : -r->nval;
            HowTensor *out = tensor_new(l->tensor->ndim, l->tensor->shape);
            for (int i = 0; i < l->tensor->nelem; i++) out->data[i] = l->tensor->data[i] + s;
            return val_tensor(out);
        }
        if (l->type == VT_NUM && r->type == VT_TENSOR) {
            HowTensor *out = tensor_new(r->tensor->ndim, r->tensor->shape);
            for (int i = 0; i < r->tensor->nelem; i++)
                out->data[i] = !strcmp(op, "+") ? l->nval + r->tensor->data[i]
                                                : l->nval - r->tensor->data[i];
            return val_tensor(out);
        }
    }
    die_at(line, 0, "unsupported tensor operation '%s'", op);
    return val_none();
}

/* Used by runtime.c for W += ... / W -= ... style updates without keeping
   tensor-specific branching in the generic augmented-assignment helper. */
Value *tensor_apply_augop(Value *old, Value *val, const char *op, int line) {
    const char *base_op;

    if (old->type != VT_TENSOR)
        die_at(line, 0, "tensor_apply_augop() requires tensor lhs");
    if (g_tape_active)
        die_at(line, 0, "tensor operations are not supported inside grad()");
    if (strcmp(op, "+=") && strcmp(op, "-=") && strcmp(op, "*=") && strcmp(op, "/="))
        die_at(line, 0, "incompatible types for tensor %s", op);

    base_op = !strcmp(op, "+=") ? "+" :
              !strcmp(op, "-=") ? "-" :
              !strcmp(op, "*=") ? "*" : "/";

    if (val->type == VT_TENSOR)
        return tensor_elemwise(old->tensor, val->tensor, base_op, line);
    if (val->type == VT_NUM) {
        if (!strcmp(base_op, "*") || !strcmp(base_op, "/"))
            return tensor_scale(old->tensor, !strcmp(base_op, "*") ? val->nval : 1.0 / val->nval, line);
        {
            double s = !strcmp(base_op, "+") ? val->nval : -val->nval;
            HowTensor *out = tensor_new(old->tensor->ndim, old->tensor->shape);
            for (int i = 0; i < out->nelem; i++) out->data[i] = old->tensor->data[i] + s;
            return val_tensor(out);
        }
    }
    die_at(line, 0, "incompatible types for tensor %s", op);
    return val_none();
}

/* ── Public builtin-facing tensor helpers ───────────────────────────────── */

/* Shared implementation behind tensor(...) so builtins.c stays as a thin
   language-facing wrapper rather than carrying the full constructor logic. */
Value *tensor_build_from_args(int argc, Value **argv) {
    if (argc == 0) die("tensor() requires at least 1 argument");
    if (argc == 1) {
        Value *arg = argv[0];
        if (arg->type == VT_LIST) {
            HowList *outer = arg->list;
            if (outer->len == 0) {
                int shape[] = {0};
                HowTensor *t = tensor_new(1, shape);
                return val_tensor(t);
            }
            if (outer->items[0]->type == VT_LIST) {
                int rows = outer->len;
                int cols = outer->items[0]->list->len;
                int shape[] = {rows, cols};
                HowTensor *t = tensor_new(2, shape);
                for (int i = 0; i < rows; i++) {
                    HowList *row = outer->items[i]->list;
                    if (row->len != cols) die("tensor(): all rows must have the same length");
                    for (int j = 0; j < cols; j++) {
                        if (row->items[j]->type != VT_NUM) die("tensor(): all elements must be numbers");
                        t->data[i * cols + j] = row->items[j]->nval;
                    }
                }
                return val_tensor(t);
            }
            {
                int shape[] = {outer->len};
                HowTensor *t = tensor_new(1, shape);
                for (int i = 0; i < outer->len; i++) {
                    if (outer->items[i]->type != VT_NUM) die("tensor(): all elements must be numbers");
                    t->data[i] = outer->items[i]->nval;
                }
                return val_tensor(t);
            }
        }
        die("tensor() requires a list argument");
    }
    if (argc == 2) {
        if (argv[0]->type != VT_LIST) die("tensor(): first arg must be shape list");
        if (argv[1]->type != VT_LIST) die("tensor(): second arg must be data list");
        HowList *shape_list = argv[0]->list;
        HowList *data_list  = argv[1]->list;
        int ndim = shape_list->len;
        int *shape = xmalloc(ndim * sizeof(int));
        for (int i = 0; i < ndim; i++) shape[i] = (int)shape_list->items[i]->nval;
        HowTensor *t = tensor_new(ndim, shape);
        free(shape);
        if (data_list->len != t->nelem)
            die("tensor(): data length %d doesn't match shape (expected %d)", data_list->len, t->nelem);
        for (int i = 0; i < t->nelem; i++) {
            if (data_list->items[i]->type != VT_NUM) die("tensor(): all data elements must be numbers");
            t->data[i] = data_list->items[i]->nval;
        }
        return val_tensor(t);
    }
    die("tensor() takes 1 or 2 arguments");
    return val_none();
}

Value *tensor_shape_value(Value *tensor_val) {
    if (tensor_val->type != VT_TENSOR) die("shape() requires a tensor");
    HowTensor *t = tensor_val->tensor;
    HowList *l = list_new();
    for (int i = 0; i < t->ndim; i++) {
        Value *v = val_num(t->shape[i]);
        list_push(l, v);
        val_decref(v);
    }
    tensor_val = val_list(l);
    list_decref(l);
    return tensor_val;
}

/* T(t) currently swaps the first two axes only, matching the existing 2D use
   throughout the samples and tests. */
Value *tensor_transpose_value(Value *tensor_val) {
    if (tensor_val->type != VT_TENSOR) die("T() requires a tensor");
    HowTensor *t = tensor_val->tensor;
    if (t->ndim < 2) die("T() requires a tensor with at least 2 dimensions");
    int m = t->shape[0], n = t->shape[1];
    int out_shape[] = {n, m};
    HowTensor *out = tensor_new(2, out_shape);
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++)
            out->data[j * m + i] = t->data[i * t->strides[0] + j * t->strides[1]];
    return val_tensor(out);
}

Value *tensor_outer_value(Value *a_val, Value *b_val) {
    if (a_val->type != VT_TENSOR || b_val->type != VT_TENSOR)
        die("outer() requires two tensors");
    HowTensor *a = a_val->tensor, *b = b_val->tensor;
    if (a->ndim != 1 || b->ndim != 1) die("outer() requires 1D tensors");
    int shape[] = {a->nelem, b->nelem};
    HowTensor *out = tensor_new(2, shape);
    for (int i = 0; i < a->nelem; i++)
        for (int j = 0; j < b->nelem; j++)
            out->data[i * b->nelem + j] = a->data[i * a->strides[0]] * b->data[j * b->strides[0]];
    return val_tensor(out);
}

/* zeros/ones/eye/sum are kept here so the tensor builtins are simple wrappers
   over a single subsystem instead of each reimplementing shape walking. */
Value *tensor_zeros_value(Value *shape_val) {
    if (shape_val->type != VT_LIST) die("zeros() requires a shape list");
    HowList *sl = shape_val->list;
    int ndim = sl->len;
    int *shape = xmalloc(ndim * sizeof(int));
    for (int i = 0; i < ndim; i++) shape[i] = (int)sl->items[i]->nval;
    HowTensor *t = tensor_new(ndim, shape);
    free(shape);
    memset(t->data, 0, t->nelem * sizeof(double));
    return val_tensor(t);
}

Value *tensor_ones_value(Value *shape_val) {
    if (shape_val->type != VT_LIST) die("ones() requires a shape list");
    HowList *sl = shape_val->list;
    int ndim = sl->len;
    int *shape = xmalloc(ndim * sizeof(int));
    for (int i = 0; i < ndim; i++) shape[i] = (int)sl->items[i]->nval;
    HowTensor *t = tensor_new(ndim, shape);
    free(shape);
    for (int i = 0; i < t->nelem; i++) t->data[i] = 1.0;
    return val_tensor(t);
}

Value *tensor_eye_value(Value *n_val) {
    if (n_val->type != VT_NUM) die("eye() requires a number");
    int n = (int)n_val->nval;
    int shape[] = {n, n};
    HowTensor *t = tensor_new(2, shape);
    memset(t->data, 0, t->nelem * sizeof(double));
    for (int i = 0; i < n; i++) t->data[i * n + i] = 1.0;
    return val_tensor(t);
}

Value *tensor_sum_value(Value *tensor_val) {
    if (tensor_val->type != VT_TENSOR) die("sum() requires a tensor or list");
    double s = 0;
    HowTensor *t = tensor_val->tensor;
    for (int i = 0; i < t->nelem; i++) s += t->data[i];
    return val_num(s);
}

Value *tensor_max_value(Value *tensor_val) {
    if (tensor_val->type != VT_TENSOR) die("max() requires a tensor");
    HowTensor *t = tensor_val->tensor;
    if (t->nelem == 0) return val_none();
    double best = t->data[0];
    for (int i = 1; i < t->nelem; i++)
        if (t->data[i] > best) best = t->data[i];
    return val_num(best);
}

Value *tensor_min_value(Value *tensor_val) {
    if (tensor_val->type != VT_TENSOR) die("min() requires a tensor");
    HowTensor *t = tensor_val->tensor;
    if (t->nelem == 0) return val_none();
    double best = t->data[0];
    for (int i = 1; i < t->nelem; i++)
        if (t->data[i] < best) best = t->data[i];
    return val_num(best);
}
