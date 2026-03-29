# howlang C Interpreter Review (Compiler-Oriented)

## Executive Summary

This interpreter is a strong foundation with a clean initial separation between frontend and runtime. However, it is currently **interpreter-first rather than language-platform-first**. The main limitation for future compiler backend reuse is that **semantics, execution, and runtime model are tightly coupled**.

The parser and AST are already reusable, but meaningful compiler reuse will require introducing a **middle layer (IR / lowering)** and reducing dependence on global runtime state.

---

## Strengths

### 1. Good high-level modular split
- `howlang_frontend` vs `howlang_runtime` is a strong starting point
- Executable is thin (good separation of concerns)

### 2. Shared AST design
- `ast.h` is used by both parser and runtime
- Enables reuse of parsing without interpreter dependency

### 3. Thoughtful parsing and diagnostics
- Token naming and error hints are strong
- Some semantic lowering (e.g., interpolation) already exists

---

## Key Issues (Blocking Compiler Reuse)

### 1. AST is execution-shaped (too much lowering too early)

Symptoms:
- AST nodes encode runtime behavior (`is_ret`, `is_throw`, `is_parallel`)
- Function nodes embed execution structure (`branches`, loop flags)
- Interpolation lowered directly during parsing

Problem:
- AST mixes syntax + semantics + execution
- Compiler backend must either accept interpreter-shaped AST or undo decisions

Recommendation:
- Keep AST as syntax-level representation
- Introduce a **normalized IR / lowering pass**

---

### 2. Frontend API is not reusable

Symptoms:
- Minimal API (`how_parse_source`)
- Errors handled via `exit()` / `longjmp`
- Global source context for diagnostics

Problem:
- Cannot embed parser in compiler, LSP, or tooling

Recommendation:
- Introduce structured APIs:
  - `ParseResult`
  - `Diagnostic`
  - `Source`
- Return errors instead of terminating process

---

### 3. Runtime model dominates semantics

Symptoms:
- `runtime_internal.h` defines language behavior
- Value system, GC, env, modules tightly coupled

Problem:
- Compiler backend forced to depend on interpreter internals

Recommendation:
- Introduce **language core layer**:
  - frontend → semantics → runtime
- Make runtime a consumer, not the definition of semantics

---

### 4. Heavy global state

Examples:
- `g_globals`, `g_module_registry`
- GC globals
- import path globals
- AD tape globals

Problems:
- Not thread-safe
- Not embeddable
- Hard to test or run multiple instances

Recommendation:
- Introduce context structs:
  - `HowContext`
  - `FrontendContext`
  - `RuntimeContext`

---

### 5. Import system is execution-driven

Symptoms:
- Import executes module immediately
- Export determined by scanning runtime environment

Problem:
- No static module graph
- Cannot compile modules independently

Recommendation:
- Split into:
  - module resolution
  - module analysis
  - module execution

---

### 6. Builtins tied to interpreter internals

Symptoms:
- Builtins call evaluator internals directly

Problem:
- Hard to reuse in compiled backend

Recommendation:
- Split builtins into:
  - language intrinsics
  - runtime services
  - interpreter-only helpers

---

### 7. Control flow is interpreter-oriented

Symptoms:
- Uses `Signal` system for return/break/throw
- Branch model embedded in evaluator logic

Problem:
- Not suitable for code generation

Recommendation:
- Add lowering step to explicit control-flow representation (CFG-like)

---

### 8. Memory model not separated

Symptoms:
- AST and runtime allocations not clearly separated
- GC used only for runtime but intertwined globally

Problem:
- Compiler needs deterministic allocation model

Recommendation:
- Use arena allocation for AST/IR
- Keep GC strictly for runtime values

---

### 9. `common.h` is overly broad

Contains:
- buffers
- diagnostics
- REPL state
- memory helpers

Problem:
- Cross-layer coupling

Recommendation:
- Split into focused modules:
  - `util.h`
  - `diag.h`
  - `source.h`

---

### 10. Parallelism and AD are runtime-specialized

Symptoms:
- Parallel loops tied to env + GC behavior
- AD implemented via global tape

Problem:
- Hard to port to compiler backend

Recommendation:
- Define backend-neutral semantics first
- Then implement in interpreter and compiler separately

---

## Recommended Refactor Plan

### Phase 1 — Frontend API cleanup
- Structured diagnostics
- No process exit in parser
- Explicit parse result objects

### Phase 2 — Introduce lowering layer
- AST → normalized IR
- Move desugaring out of parser

### Phase 3 — Remove global state
- Introduce context objects
- Thread contexts through APIs

### Phase 4 — Module system redesign
- Separate load / analyze / execute

### Phase 5 — Define intrinsic layer
- Stabilize builtin interface

### Phase 6 — Add bytecode backend (recommended first)
- Easier than native codegen
- Reuses interpreter semantics

---

## Target Architecture

```
frontend/
  lexer
  parser
  diagnostics
  AST

semantics/
  symbol resolution
  lowering
  module graph

ir/
  HIR / bytecode IR

runtime/
  Value, Env, GC
  interpreter

backend/
  bytecode VM or native codegen

cli/
  REPL and driver
```

---

## Final Verdict

The project is a **well-structured interpreter**, but not yet a reusable language platform.

To support a compiler backend effectively, the key is:

> Introduce a clean semantic middle layer and remove interpreter-specific assumptions from the frontend.

Once that is done, most of the existing code becomes reusable rather than something the compiler must work around.

---

## Optional Next Step

A practical next move would be to design a **minimal normalized IR** and refactor one feature (e.g., function calls or loops) through it as a pilot.

