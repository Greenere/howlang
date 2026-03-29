# Howlang — More Tensor Syntax Proposal

## Context

The first tensor proposal solved the biggest performance problem in
`samples/mlp.how`: lists and list-of-lists no longer stand in for vectors and
matrices, and the code can now use `tensor(...)`, element-wise arithmetic, and
`@` for matmul.

That already makes the linear algebra much cleaner:

```how
z = W @ x + b
grad_x = T(W) @ grad_z
W -= lr * outer(grad_z, x_in)
```

But one part of the MLP still looks more manual than mathematical: activation
application and activation derivatives. The current sample still has to loop
through tensor elements, call scalar functions one at a time, build temporary
lists, and wrap them back into tensors.

This proposal is about the next layer of syntax improvements: not new tensor
storage, but more elegant tensor-oriented language forms.

The guiding constraint is the core gist of Howlang from `readme.md`:

- small, expressive language
- everything is a function
- prefer a few composable primitives over a large framework surface

So the question is not just "what would make `cnn.how` shorter?", but "what is
general enough to belong in Howlang itself?"

---

## What still looks awkward today

In the current `mlp.how`, the forward and backward passes are partly tensor
native and partly scalar-loop adapters:

```how
forward: (x){
    z = W @ x + b,
    var out = list(),
    (i=0:len(z)){ push(out, act_s(z(i))) }(),
    h = tensor(out),
    :: h
}

backward: (grad_h, x_in, lr){
    var dz = list(),
    (i=0:len(z)){ push(dz, d_act(z(i))) }(),
    var grad_z = grad_h * tensor(dz),
    var grad_x = T(W) @ grad_z,
    W -= lr * outer(grad_z, x_in),
    b -= lr * grad_z,
    :: grad_x
}
```

This works, but it has two costs:

1. **Syntax cost**: the code stops reading like the math.
2. **Runtime cost**: every training step still allocates temporary lists for
   activations and activation gradients.

---

## Proposal 1: Unary numeric functions broadcast over tensors

If a function is numeric and scalar-valued, calling it on a tensor should apply
it element-wise and return a tensor of the same shape.

### User-facing behavior

```how
var relu_s = (x){
    x > 0 :: x,
    :: 0
} grad (x, g){
    x > 0 :: g,
    :: 0
}

var sigmoid_s = (x){ :: 0.5 + 0.5 * x / (1 + abs(x)) }

relu_s(tensor({-1.0, 2.0, -3.0}))      # [0, 2, 0]
sigmoid_s(tensor({0.0, 1.0, -1.0}))    # element-wise tensor result
```

### What `mlp.how` becomes

```how
forward: (x){
    z = W @ x + b,
    h = act_s(z),
    :: h
}
```

### Why this helps

- It makes tensor code look like scalar code.
- It preserves the existing mental model: functions stay plain callables.
- It removes the need for manual `list()` + `push(...)` activation loops.

### Scope boundary

This should apply only when:

- the argument is a tensor
- the function is called with one argument
- the function returns a number for a numeric scalar input

Non-numeric functions and multi-argument functions should keep their current
behavior unless explicitly extended later.

---

## Proposal 2: `grad()` should work element-wise on tensor-valued inputs

Today `grad()` is still fundamentally scalar-oriented. That is why `mlp.how`
has to build `dz` with a manual loop, even though `z` is already a tensor.

The syntax win would be to allow:

```how
var d_act = grad(act_s)
var grad_z = grad_h * d_act(z)
```

where `z` is a tensor and `d_act(z)` means "apply the scalar derivative
element-wise".

### What `mlp.how` becomes

```how
backward: (grad_h, x_in, lr){
    var grad_z = grad_h * d_act(z),
    var grad_x = T(W) @ grad_z,
    W -= lr * outer(grad_z, x_in),
    b -= lr * grad_z,
    :: grad_x
}
```

### Why this helps

- It removes the most awkward remaining loop in the sample.
- It keeps the current `grad()` model instead of introducing a new derivative
  API just for tensors.
- It gives a smooth migration path from scalar examples to tensor examples.

### Important boundary

This proposal is **not** "full tensor autodiff for arbitrary tensor programs".
It is the smaller, syntax-focused version:

- `grad(f)` for scalar `f`
- if called on a tensor, map the scalar derivative element-wise

That is enough to make MLP-style code much cleaner without forcing a complete
autodiff redesign in the same step.

---

## Proposal 3: Add an explicit tensor map form

If automatic broadcasting of arbitrary user-defined functions feels too magical,
the language could instead add an explicit tensor mapping form.

Possible shapes:

```how
map(z, act_s)
map_tensor(z, act_s)
z |> map(act_s)
```

Any of these would already be a major readability improvement over:

```how
var out = list()
(i=0:len(z)){ push(out, act_s(z(i))) }()
h = tensor(out)
```

### Why this may be preferable

- More explicit than automatic broadcasting
- Easier to explain in the language reference
- Easier to limit to tensors without changing normal function-call semantics

### Why it is less elegant than Proposal 1

It still introduces a "special helper syntax" instead of letting tensor code
reuse the same plain function-call form as scalar code.

For MLP readability, automatic tensor broadcast of unary numeric functions is
the nicer end state.

---

## Proposal 4: Tensor-native activations as builtins

Another option is to provide explicit builtin activations:

```how
relu(z)
sigmoid(z)
```

This would let examples become concise immediately:

```how
h = relu(W @ x + b)
```

### Pros

- Very clean syntax
- Easy to optimize in C
- No ambiguity about supported operations

### Cons

- More special cases in the language
- Less general than tensor-aware function calls
- Does not help custom activations unless more builtins keep being added

This is a pragmatic option, but not the most compositional one.

---

## CNN-driven follow-up ideas

The CNN sample surfaces a second class of awkwardness beyond MLP activations:
spatial operations such as convolution and pooling still require nested scalar
loops in user code.

Some of those ideas fit Howlang well; others solve the sample but pull the
language toward a model-library style that does not match its current core.

### A. Axis-aware reductions are general and aligned

One thing `cnn.how` still has to write manually is "reduce along one axis":

```how
var out = list()
(f=0:len(maps)){
    push(out, sum(maps(f)) / len(maps(f)))
}()
:: tensor(out)
```

This suggests a more general primitive such as:

```how
sum(maps, 1)
mean(maps, 1)
max(maps, 1)
```

or perhaps named builtins like:

```how
sum_axis(maps, 1)
mean_axis(maps, 1)
max_axis(maps, 1)
```

### Why this fits Howlang

- It is not ML-specific.
- It generalizes naturally from existing builtins like `sum`, `max`, and `min`.
- It preserves the "everything is a function" model.
- It helps many tensor programs, not just CNNs.

### Why this helps `cnn.how`

Global average pooling becomes something like:

```how
var vec = mean(maps, 1)
```

instead of a hand-written loop over filters.

---

### B. Tensor slicing / window views are general and aligned

Another missing piece is expressing local windows cleanly. Convolution today
must be written as scalar index arithmetic:

```how
(k=0:ks){ s += kernels(f)(k) * x(t+k) }()
```

If tensors had better slicing or window/view primitives, code could become more
declarative without introducing a special "neural net" feature.

Possible directions:

```how
x(t:t+ks)              # already list-like slice syntax, extended to tensors
window(x, t, ks)       # explicit helper returning a tensor view
windows(x, ks)         # all sliding windows, shape {T_out, ks}
```

### Why this fits Howlang

- It extends existing list-call/slice ideas instead of inventing a new DSL.
- Views are a natural tensor concept, not a CNN-only concept.
- It composes with existing primitives such as `@`, `sum`, and `map`.

### Why this helps `cnn.how`

Even before a builtin convolution exists, the inner loop could move toward:

```how
var patch = x(t:t+ks)
var s = kernels(f) @ patch + bias(f)
```

That is both clearer and a better stepping stone toward runtime optimization.

---

### C. Batched tensor operations are general, but should stay minimal

Many numerical programs want to operate on a whole batch of rows at once, not
one vector at a time. That applies to MLPs, CNNs, statistics, and simulation.

Good general forms:

- matmul that naturally handles more dimensions
- reductions over selected axes
- broadcasting that works across batch dimensions

### Why this fits Howlang

- Batch structure is just more tensor structure.
- It avoids adding separate "training loop" concepts to the language.

### Constraint

This should be exposed as tensor semantics, not as special training APIs like
`fit`, `DataLoader`, or optimizer objects.

---

### D. First-class `conv1d(...)` is useful but only conditionally aligned

A builtin such as:

```how
conv1d(x, kernels, bias)
```

would make `cnn.how` much shorter and much faster.

### Why it is tempting

- Convolution is the single hottest nested-loop pattern in the sample.
- It is hard to express efficiently in userland without more tensor features.
- It would immediately make the example read more like the math.

### Why it is only partially aligned

- It is much more domain-specific than `sum`, `shape`, `T`, or `@`.
- It starts to move the language from "small tensor core" toward "ML operator catalog".
- Once `conv1d` exists, pressure grows for `conv2d`, padding modes, stride,
  dilation, pooling, softmax, normalization, and so on.

### Recommendation

Do **not** make `conv1d` the next syntax step.

It is reasonable later as a performance builtin if:

- the more general tensor pieces still leave convolution awkward
- the implementation can stay small and principled
- it is framed as a numeric primitive, not as a neural-net subsystem

---

### E. Pooling-specific builtins are probably too narrow

Things like:

```how
global_avg_pool(maps)
max_pool1d(x, k)
avg_pool1d(x, k)
```

solve real problems, but they are less general than axis reductions and window
views.

### Why they are less aligned

- They are closer to "deep learning convenience layer" than core language.
- Most of their value can be recovered from better reductions + slicing.
- They increase builtin count faster than expressive power.

### Recommendation

Prefer:

- `mean(..., axis)`
- `max(..., axis)`
- window/view helpers

before adding pooling-specific builtins.

---

### F. Tensor-native nonlinear builtins are useful, but should be judged carefully

Builtins like:

```how
relu(x)
sigmoid(x)
tanh(x)
softmax(x)
```

would help examples a lot.

### What is aligned

- `relu`, `sigmoid`, and `tanh` are defensible as common numeric transforms.
- They can be optimized in C.
- They pair naturally with tensor broadcasting.

### What is less aligned

- A long list of ML-only activation builtins would make the language surface
  feel library-like.
- `softmax` is more specialized and tied to particular model patterns.

### Recommendation

Prefer the more general rule first:

- unary numeric functions broadcast over tensors

Then add only a very small number of builtin nonlinearities if performance or
clarity still justifies them.

---

## What seems most generalizable

From both `mlp.how` and `cnn.how`, the ideas that feel general enough for
Howlang itself are:

1. unary numeric function broadcast over tensors
2. element-wise scalar `grad()` over tensor inputs
3. axis-aware reductions (`sum`, `mean`, `max`, maybe `min`)
4. tensor slicing and window/view operations
5. batch-friendly tensor semantics as a natural extension of shapes/broadcasting

These all:

- apply well beyond machine learning
- preserve the "everything is a function" model
- compose from a small primitive set
- improve both elegance and runtime opportunities

---

## What seems less core to Howlang

The ideas that feel less aligned with the language's core gist are:

1. a growing catalog of ML-specific builtins
2. pooling-specific syntax as a first move
3. optimizer/training abstractions
4. model-definition DSL features

Those may be useful someday, but they belong later, if at all. They solve
example-specific pain points without strengthening the language's general
compositional core.

---

## Proposal 5: Keep object syntax stable; improve tensor ergonomics first

The current layer/class syntax:

```how
var Layer = [W, b, act_s]{
    ...
}
```

is not the main readability problem in `mlp.how`. The bigger issue is the
scalar-loop ceremony inside otherwise tensor-native code.

So if the goal is "make the MLP look elegant", the priority order should be:

1. tensor-aware unary function application
2. tensor-aware scalar `grad()` application
3. axis-aware reductions
4. tensor slicing / window views
5. maybe an explicit tensor-map form as an intermediate step
6. only then consider broader syntax changes for classes/modules

---

## Recommended path

The smallest syntax change with the biggest payoff is:

### Step 1

Allow unary numeric functions to broadcast over tensors.

This alone turns:

```how
var out = list()
(i=0:len(z)){ push(out, act_s(z(i))) }()
h = tensor(out)
```

into:

```how
h = act_s(z)
```

### Step 2

Let `grad(f)` for scalar `f` also broadcast element-wise over tensors.

This turns:

```how
var dz = list()
(i=0:len(z)){ push(dz, d_act(z(i))) }()
var grad_z = grad_h * tensor(dz)
```

into:

```how
var grad_z = grad_h * d_act(z)
```

### Step 3

Add axis-aware reductions and tensor window/view operations.

This is the natural next generalization for CNN-like code:

```how
var vec = mean(maps, 1)
var patch = x(t:t+ks)
```

### Step 4

Only if Step 1 feels too implicit, add an explicit tensor-map builtin instead.

---

## End-state example

With these improvements, the MLP layer would finally read almost exactly like
the underlying equations:

```how
var Layer = [W, b, act_s]{
    W: W,
    b: b,
    act_s: act_s,
    d_act: grad(act_s),
    z: list(),
    h: list(),

    forward: (x){
        z = W @ x + b,
        h = act_s(z),
        :: h
    },

    backward: (grad_h, x_in, lr){
        var grad_z = grad_h * d_act(z),
        var grad_x = T(W) @ grad_z,
        W -= lr * outer(grad_z, x_in),
        b -= lr * grad_z,
        :: grad_x
    }
}
```

That is the real target: not just faster tensors, but tensor code that reads as
directly as the math it implements.

For CNNs, the analogous end-state is not "add lots of NN helpers", but to make
the general tensor language strong enough that local-window and axis-reduction
code also reads directly as the math it implements.
