# Howlang — Tensor Ops Proposal

## Context

The current tensor layer already covers the first big step:

- numeric tensor values
- element-wise arithmetic
- scalar broadcast
- matrix multiplication via `@`
- transpose / outer / zeros / ones / eye / sum

That is enough to make `samples/mlp.how` and `samples/cnn.how` much cleaner than
their list-based predecessors.

But the newer samples, especially [transformer.how](/Users/haoyangli/projects/howlang/samples/transformer.how),
show the next limitation clearly:

- the math is often tensor-shaped
- the code is still forced to build many row sequences manually with `list()`
  and `push(...)`
- reductions, masks, and row-wise transforms are still expressed with explicit
  loops and scalar helpers

So the problem is no longer “Howlang lacks tensors.”
The problem is:

**Howlang lacks a small layer of tensor-sequence operations above raw `@` and
element-wise arithmetic.**

This proposal is about that next layer.

---

## Design goals

The goal is not to turn Howlang into NumPy or PyTorch.

The goal is to add a **small, sharp** set of tensor/sequence operations that:

- fit Howlang’s explicit style
- remove the most repetitive list-building scaffolding
- let tensor code read more like the math
- stay implementable in C without forcing a huge tensor framework surface

The guiding constraints:

- keep the core small
- prefer a few composable builtins over a broad API
- avoid magical broadcasting rules for arbitrary user functions
- optimize for the patterns that already appear in repo samples

---

## What the current samples reveal

### 1. Sequence assembly is still manual

In [cnn.how](/Users/haoyangli/projects/howlang/samples/cnn.how) and [transformer.how](/Users/haoyangli/projects/howlang/samples/transformer.how),
many intermediate values are “a list of tensor rows.”

That leads to repeated patterns like:

```how
var rows = list()
(i=0:n){
    push(rows, some_row)
}()
:: tensor(rows)
```

This is conceptually:

- map over row indices
- stack the resulting rows into a tensor

but today the language has no direct operation for that.

### 2. `@` is useful, but not enough

Matrix multiply helps for local pieces like:

```how
W @ x
Q @ T(K)
W2 @ h
```

But transformer-style code also needs:

- row-wise softmax
- causal masking
- row-wise weighted sums
- batched row construction
- accumulation into row tensors

Those are not expressible as plain `@` alone.

### 3. Row-wise reductions are awkward

Today we have `sum(tensor)` as a full reduction to one scalar.

But samples often want:

- sum over each row
- mean over each row
- max over each row

Without axis-aware reductions, code falls back to loops or list-building.

### 4. Masking is missing as a tensor-native concept

The transformer sample currently has to construct causal behavior row by row.

What is conceptually needed is:

- build scores
- apply a causal mask
- row-softmax

The absence of mask-aware tensor ops is one reason attention stays loop-heavy.

---

## Proposed operation families

This proposal recommends four small families of operations.

### Family 1 — Tensor row construction

#### `stack(rows)`

Build a tensor by stacking a list of equal-length row tensors or numeric lists.

Example:

```how
var rows = map(range(T), (i){ :: token_embed(ids(i)) + pos_embed(i) })
var x = stack(rows)
```

This replaces the common `list() + push(...) + tensor(...)` pattern.

Why it matters:

- makes “map then tensorize” explicit
- avoids manual row-by-row list assembly
- gives the runtime one place to optimize row stacking

#### `unstack(t)`

Return a list of row tensors.

This is not strictly necessary for v1, but it pairs naturally with `stack(...)`
and may simplify code that needs row iteration while keeping tensor semantics.

---

### Family 2 — Axis-aware reductions

#### `sum(x, axis=...)`
#### `mean(x, axis=...)`
#### `max(x, axis=...)`

Current `sum(x)` can stay as the full reduction form.
The extension is to support row-wise and possibly column-wise reduction.

Likely initial scope:

- `axis=0` and `axis=1` for 2D tensors
- perhaps only `axis=1` in v1 if we want to stay very small

Examples:

```how
row_sums = sum(scores, axis=1)
row_means = mean(maps, axis=1)
```

Why it matters:

- matches common ML math
- removes many small explicit loops
- makes pooling operations more natural

---

### Family 3 — Row-wise transforms

#### `row_map(t, fn)`

Apply `fn` to each row tensor and stack the results.

Example:

```how
var y = row_map(x, (row){ :: W @ row + b })
```

This is different from current `map(tensor, fn)`, which is element-wise and
numeric-only.

`row_map(...)` would operate at the row level, not the scalar element level.

Why it matters:

- many model layers are naturally “apply this per row”
- keeps row-wise tensor code explicit
- avoids overloading ordinary `map(...)` with another meaning

#### `row_zip_map(a, b, fn)`

Optional extension for v2.
Useful when two row-aligned tensors need to be combined row by row.

---

### Family 4 — Masking and attention-friendly ops

This is the family most clearly motivated by the transformer sample.

#### `masked_fill(scores, mask, value)`

Set entries where `mask` is false to `value`.

This allows attention-style code to read like:

```how
scores = Q @ T(K)
scores = masked_fill(scores, causal_mask(T), -1e9)
weights = row_softmax(scores)
ctx = weights @ V
```

#### `causal_mask(n)`

Return an `n x n` lower-triangular mask.

This is intentionally specific.
Causal attention is common enough, and a builtin is much cleaner than hand-built
triangle loops in sample code.

#### `row_softmax(t)`

Apply softmax independently to each row of a 2D tensor.

This is one of the clearest missing operations in the current transformer code.

#### `attention(q, k, v, causal=true)`

This is the most aggressive option in the proposal and may be too high-level
for the first pass.

It would fuse:

- score computation
- optional masking
- row-softmax
- weighted value aggregation

The benefit is very clean code:

```how
ctx = attention(Q, K, V, causal=true)
```

The downside is that it jumps from primitive tensor ops to a domain-specific ML
operation. My recommendation is to delay this until the smaller building blocks
exist first.

---

## Suggested initial set

If we want the highest leverage with the smallest surface area, the best first
set is probably:

1. `stack(rows)`
2. `row_map(t, fn)`
3. `sum(x, axis=1)` and `mean(x, axis=1)`
4. `row_softmax(t)`
5. `causal_mask(n)`
6. `masked_fill(scores, mask, value)`

Why this set:

- it directly targets the repetitive patterns in current samples
- it keeps attention code explicit instead of collapsing everything into one
  mega-builtin
- it improves both CNN and transformer code, not just one domain

---

## Non-goals

This proposal does **not** aim to add:

- full NumPy-style broadcasting semantics
- arbitrary n-dimensional axis algebra
- a large neural-network library
- automatic graph-level optimization
- generic higher-order tensor autodiff rules

Those may come later, but they are not needed to get a large readability win
right now.

---

## What current samples would look like

### CNN direction

Current style in [cnn.how](/Users/haoyangli/projects/howlang/samples/cnn.how):

```how
var rows = list()
(t=0:T_out){
    push(rows, map(range(ks), (k){ :: x(t + k) }))
}()
:: tensor(rows)
```

With `stack(...)`:

```how
:: stack(map(range(T_out), (t){ :: map(range(ks), (k){ :: x(t + k) }) }))
```

With a future `windows(x, ks)` helper, even shorter:

```how
:: windows(x, ks)
```

### Transformer direction

Current style in [transformer.how](/Users/haoyangli/projects/howlang/samples/transformer.how):

```how
var scores = list()
(j=0:i+1){
    push(scores, (q(i) @ k(j)) / scale)
}()
var weights = softmax(scores)
```

With tensor row ops:

```how
scores = (Q @ T(K)) / scale
scores = masked_fill(scores, causal_mask(T), -1e9)
weights = row_softmax(scores)
ctx = weights @ V
```

That is much closer to the standard transformer math.

---

## Implementation strategy

The recommended implementation order is:

1. Add `stack(rows)` and maybe `unstack(t)` in `tensor.c`
2. Add row-wise reductions: `sum(..., axis=1)`, `mean(..., axis=1)`
3. Add `row_softmax(t)` and `causal_mask(n)`
4. Add `masked_fill(scores, mask, value)`
5. Revisit whether a higher-level `attention(...)` builtin is still necessary

This keeps the tensor subsystem incremental and testable.

---

## Recommendation

Howlang does not need a huge tensor API.
It does need a few more tensor-sequence operations if ML-shaped code is going to
feel native.

The right next step is not “more operator overloading.”
It is a compact layer of builtins for:

- stacking row results
- row-wise reductions
- row-wise transforms
- masking
- row-wise softmax

That is enough to make the existing CNN and transformer samples read more like
their intended mathematics without pushing Howlang into framework territory.
