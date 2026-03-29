# Howlang — Chained Closure Proposal

## Motivation

While exploring `grad` and other higher-order patterns, a more general syntax
idea emerged:

```how
var f =
    (x){ ... }
    wrap (cfg1, cfg2){ ...f... }
    more (cfg3){ ...f... }
```

This is **not** about defining multiple related functions in one place.

It is about expressing a sequence of **higher-order wrappers** locally, where
each trailing stage captures the callable produced by the previous stage.

The appeal is that many useful Howlang patterns already want this shape:

- logging / tracing
- retry / fallback wrappers
- validation / sanitization wrappers
- memoization
- timing / instrumentation
- staged specialization

Today we can write all of these explicitly with intermediate names, but the
result is mechanically verbose and splits one conceptual pipeline into several
unrelated declarations.

---

## Core idea

Allow a function-valued expression to be followed by one or more **trailing
wrapper clauses**.

Informally:

```how
var f =
    BASE
    WRAP1 (config...){ body referring to f }
    WRAP2 (config...){ body referring to f }
```

Each trailing clause:

1. receives its own independent configuration parameters
2. captures the callable produced so far
3. produces a new callable
4. becomes the input to the next clause

The final bound name is the last callable in the chain.

This is best thought of as **syntactic sugar for staged closure rebinding**.

---

## Important clarification

This proposal is **not** about:

- companion functions attached to a primary function
- metadata or protocol slots
- method-like sidecars

It is specifically about **progressive higher-order transformation** of a
callable value.

That is why trailing clause parameters may be completely different from the
primary callable's parameters.

For example:

```how
var fetch =
    (url){ :: read(url) }
    trace ("fetch"){
        print("[trace] fetch")
        :: fetch(url)
    }
    retry (3, 100){
        :: fetch(url)
    }
```

Here:

- `url` belongs to the base callable's runtime call signature
- `"fetch"`, `3`, and `100` belong to wrapper construction

So the syntax is not “same function, multiple bodies.”
It is “build a function, then wrap it with configured higher-order closures.”

---

## Proposed syntax

### Shape

```how
var name =
    (primary_params){ primary_body }
    tail_name_1 (wrapper_params_1){ wrapper_body_1 }
    tail_name_2 (wrapper_params_2){ wrapper_body_2 }
```

Minimal single-line form:

```how
var f = (x){ :: x * 2 } log ("double"){ print("log"), :: f(x) }
```

### Grammar intuition

This is intentionally descriptive, not final parser grammar:

```text
chained_closure
    := callable_expr chained_tail*

chained_tail
    := IDENT "(" param_list? ")" "{" body "}"
```

with the constraint that the base expression must be callable-valued in source
shape, at least in the initial version.

---

## Desugaring model

The feature should have one simple mental model.

For a chain like:

```how
var f =
    A
    g (p1, p2){ B }
    h (q1){ C }
```

the conceptual desugaring should be:

```how
var __stage0 = A
var __stage1 = <evaluate g-clause with previous stage = __stage0>
var __stage2 = <evaluate h-clause with previous stage = __stage1>
var f = __stage2
```

The trailing body is evaluated in an environment where the original binding
name (`f`) refers to the **previous stage**, not yet to the final value.

That is the simplest “progressive wrapping” interpretation.

---

## Examples

### Example 1 — logging wrapper

```how
var square =
    (x){ :: x * x }
    log ("square"){
        print("[log] square(" + str(x) + ")")
        :: square(x)
    }
```

Reading:

- define `square₀(x) = x * x`
- define `square₁` that logs then calls `square₀`
- final `square` is `square₁`

### Example 2 — validation then clamping

```how
var safe_sqrt =
    (x){ :: sqrt(x) }
    validate ("non-negative"){
        x < 0 :: !! "negative input"
        :: safe_sqrt(x)
    }
    clamp (0.0){
        x < 0 :: safe_sqrt(0.0)
        :: safe_sqrt(x)
    }
```

The second wrapper should see the already-validated behavior if we use
previous-stage capture semantics.

### Example 3 — memoization shape

```how
var fib =
    (n){
        n <= 1 :: n,
        :: fib(n - 1) + fib(n - 2)
    }
    memo (256){
        :: fib(n)
    }
```

This example is important because it exposes the main semantic fork discussed
below: should `fib` inside the wrapper refer to the previous stage or the final
wrapped function?

---

## The key semantic fork

There are two plausible meanings for the name captured inside a trailing clause.

### Option A — previous-stage capture

Inside each trailing body, the binding name refers to the callable produced by
the immediately previous stage.

This gives a clean desugaring story:

```how
var f0 = BASE
var f1 = WRAP1(f0)
var f2 = WRAP2(f1)
var f  = f2
```

Pros:

- simplest mental model
- easiest interpreter implementation
- easy to reason about as local rebinding
- no magic fixed-point behavior

Cons:

- wrappers like memoization may not intercept recursive self-calls in the way
  users expect

### Option B — final-name capture

Inside each trailing body, the binding name refers to the final fully wrapped
callable.

Pros:

- recursive decorators like memoization become more powerful
- feels natural in some recursive examples

Cons:

- much harder to explain
- no longer simple staged rebinding
- introduces fixed-point / self-reference semantics
- easier to create accidental infinite recursion or confusing wrapper order

### Recommendation

Start with **Option A: previous-stage capture**.

It is more Howlang-like:

- explicit
- local
- closure-based
- easy to desugar

If recursive decorator semantics are important later, they can be handled
deliberately with another mechanism rather than making the base feature harder
to reason about.

---

## Why independent wrapper args matter

This is what makes the feature genuinely useful.

Trailing clauses are not just alternate bodies of the same function signature.
They are **wrapper constructors** with their own configuration interface.

Examples:

- `trace(label)`
- `memo(capacity)`
- `retry(times, delay_ms)`
- `throttle(ms)`
- `timeout(ms)`

This pushes the design firmly toward:

- higher-order composition
- closure capture
- wrapper staging

and away from:

- branch overloading
- multi-methods
- metadata attachments

---

## Current Howlang approximation

Today the same behavior requires explicit intermediate names:

```how
var base = (x){ :: x * 2 }
var logged = (x){
    print("log")
    :: base(x)
}
var clamped = (x){
    x < 0 :: logged(0),
    x > 10 :: logged(10),
    :: logged(x)
}
```

Or, if we want the final public name only:

```how
var f0 = (x){ :: x * 2 }
var f1 = (x){ print("log"), :: f0(x) }
var f  = (x){
    x < 0 :: f1(0),
    x > 10 :: f1(10),
    :: f1(x)
}
```

This works, but it is:

- mechanically named
- split across unrelated declarations
- less local than the conceptual wrapper chain

---

## Interaction with existing Howlang style

Why this idea may fit Howlang:

- it is still closure-based, not object-system-based
- it keeps code local and readable
- it preserves explicit semantics if desugared cleanly
- it expresses staged transformation without introducing a large new runtime model

Why it may not fit if pushed too far:

- arbitrary chain names could become visually noisy
- final-name capture semantics would make it magical
- if generalized beyond wrappers, it starts looking like a metaobject system

So the feature should stay narrowly about **callable wrapping**.

---

## Suggested initial scope

If explored experimentally, keep the first version tight:

1. Base must be a function literal or other obvious callable expression
2. Trailing clauses are only allowed immediately in a `var` initializer
3. Use previous-stage capture semantics
4. The final result is always a callable
5. No extra reflection or runtime protocol lookup in v1

This keeps the feature understandable and implementation effort bounded.

---

## Parser / runtime sketch

This is intentionally high-level.

### Parser

Add a new post-callable parse shape after a function-valued initializer:

- parse base callable expression
- while the next tokens look like `IDENT ( ... ) { ... }`, parse a chained tail
- lower the chain to either:
  - explicit nested AST nodes, or
  - desugared nested function values immediately in the parser

### Runtime

Preferred implementation strategy:

- desugar early
- avoid adding a new runtime value kind

If the parser lowers chained closures into ordinary nested functions with
ordinary captures, the runtime stays almost unchanged.

That is a strong argument for the feature: it can be mostly a frontend sugar.

---

## Open questions

1. Should the trailing names (`log`, `memo`, `retry`) be semantically visible,
   or are they syntax markers only?

2. Should the feature accept only function literals as the base, or any
   callable-valued expression?

3. How exactly should wrapper-configuration args be scoped relative to the
   wrapped function’s call args?

4. Do we want any special affordance for recursive decorators later, or is
   previous-stage capture enough?

5. Does this feature stand on its own, or should it wait until there is a
   stronger body of wrapper-heavy sample code?

---

## Recommendation

This idea is promising enough to keep exploring.

The strongest version is:

- **not** companion-function attachment
- **not** metadata/protocol decoration
- **yes** to staged higher-order closure wrapping with independent wrapper args

If we prototype it, we should do so with:

- previous-stage capture
- narrow syntax
- parser-level desugaring

That gives the feature the best chance of feeling like a natural Howlang
extension rather than a new source of semantic magic.
