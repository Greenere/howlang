# Howlang

A small, expressive programming language by Haoyang Li (2022).  
This repository contains a C interpreter (`c_interpreter/`) and an optional
self-hosting meta-interpreter written in Howlang itself (`samples/how_interpreter/`).

---

## Quick Start

```bash
# Build (requires CMake 3.16+ and a C compiler)
cd c_interpreter
cmake -S . -B build
cmake --build build

# Run a file
./build/howlang program.how

# Interactive REPL
./build/howlang

# Meta-interpreter REPL (Howlang interpreting Howlang)
./build/howlang samples/how_interpreter/how_meta.how
```

---

## Language Reference

### Primitives & Variables

```
var a = 42        # Number
var b = "hello"   # String  (double or single quotes)
var c = true      # Bool
var d = none      # None
var x = 0;        # Semicolons are optional
```

### Collections

**List** — zero-indexed, called like a function:

```
var nums = {10, 20, 30, 40, 50}
nums(0)       # 10
nums(1:3)     # {20, 30}   — slice [1, 3)
nums(:2)      # {10, 20}   — slice from start
nums(2:)      # {30, 40, 50} — slice to end
nums(99)      # none        — out of bounds returns none
{42}          # single-element list
len(nums)     # 5

# List concatenation
{1,2} + {3,4}  # {1,2,3,4}
```

**Map** — any-keyed dictionary, called like a function with a dynamic key:

```
var m = {"name": "Alice", "age": 30}
m("name")        # "Alice"
m("missing")     # none  — missing keys return none, never crash

# Dynamic key write — same syntax as reading, just assign to it:
var scores = map()
scores("alice") = 95
scores("bob")   = 87
scores("alice") += 5   # augmented assignment works too
print(scores("alice")) # 100

# Dot syntax still works for literal key names:
m.name           # "Alice"
m.name = "Bob"   # update field
```

**List** index write uses the same call-assignment syntax:

```
var lst = {10, 20, 30}
lst(1) = 99      # [10, 99, 30]
lst(0) += 5      # [15, 99, 30]
lst(9)           # none — out of bounds returns none
```

### Functions

Functions are **branch maps** — a list of `condition :: return` pairs.
The first truthy condition fires; `:: expr` is the unconditional default.

```
var abs_val = (n){
    n >= 0 :: n,
    :: -n
}

# Local variables with var
var hyp = (a, b){
    var a2 = a * a,
    var b2 = b * b,
    :: sqrt(a2 + b2)
}
```

### Branching (if-else)

No `if` keyword — use an anonymous function called immediately:

```
(){
    x > 0 :: print("positive"),
    x < 0 :: print("negative"),
    :: print("zero")
}()
```

### Loops

#### Unbounded loop `(:){ }`

Runs forever until `::` (return/break) is hit:

```
var i = 0
(:){
    i >= 10 :: i,    # break and return i
    i += 1           # side effect — keep looping
}()
```

#### Unbounded loop with explicit `break`: `(:)= { }`

The `=` suffix auto-executes the loop. Use `break` to exit without a return value:

```
var i = 0
(:)={
    i > 4: break,    # explicit break — exits the loop
    i += 1
}
# i is now 5
```

#### For-range loop: `(var=start:stop){ }()`

Iterates `var` over `[start, stop)`. The variable is declared inline in the
parameter position — consistent with how parameters are always written in `( )`.

The for-range is a **callable value**: explicit `()` is required to run it,
exactly like the unbounded `(:){ }()` loop. `::` exits immediately with a
return value, same as in any other loop or function:

```
# Loop as a statement — run it, then use the result
var total = 0
(i=0:5){ total += i }()
# total == 10

# Early exit: :: exits on the first match
var first_big = (i=0:10){ i * i > 20 :: i }()
# first_big == 5  (5*5=25 > 20)

# Store the loop, call it later
var my_loop = (i=0:3){ print(str(i)) }
my_loop()   # prints 0, 1, 2
my_loop()   # prints 0, 1, 2 again
```

#### Loop control: `break` and `continue`

Inside any loop, `break` exits immediately and `continue` skips the rest of the
current iteration and advances to the next:

```
# Skip even numbers, collect odds
var odds = list()
(i=0:10){
    i % 2 == 0: continue,   # skip evens
    push(odds, i)
}()
# odds == {1, 3, 5, 7, 9}

# Multiple guards — continue makes each guard independent and flat
(i=0:len(items)){
    items(i) == none: continue,   # skip nulls
    items(i) < 0:    continue,   # skip negatives
    push(results, items(i) * 2)  # only valid, non-negative items reach here
}()
```

`break` and `continue` work the same way in `(:){ }()` unbounded loops.

---

### Classes

Declared with `[params]{ field: value, ... }`, instantiated with `Name[args]`.
Field names can be bare identifiers (used as string keys) or expressions:

```
var person = [name, age]{
    name: name,          # field "name" = param name
    age: age,
    greet: (phrase){
        :: print("Hi " + phrase + " I am " + name)
    }
}

var p = person["Alice", 30]
p.name          # "Alice"
p.greet("there")  # Hi there I am Alice
```

### Error Handling

Howlang has two dedicated operators for error handling: `!!` (throw) and `catch` (recover). They are siblings of `:` and `::` — no `try` block is needed.

#### `!!` — throw

`!!` raises an error carrying any value. It can be unconditional or conditional:

```
# Unconditional throw — always raises
!! "something went wrong"

# Conditional throw — raises only when condition is truthy (same line rule)
b == 0 !! "division by zero"
n < 0  !! "negative: " + str(n)
```

Inside a function, error values propagate upward exactly like return values — they unwind the call stack until a `catch` handles them or the program exits with an error message.

```
var safe_div = (a, b){
    b == 0 !! "division by zero"
    :: a / b
}
```

#### `catch` — recover

`catch` is a left-associative binary infix operator. It evaluates the left side; if that raises an error the handler on the right is called with the error value, and `catch` returns the handler's result. If no error occurs, the left-side value passes through untouched.

```
var result = safe_div(10, 0) catch (e){ :: -1 }
# → -1  (error caught, handler returns fallback)

var ok = safe_div(10, 2) catch (e){ :: -1 }
# → 5   (no error, catch handler never called)
```

The error value can be any Howlang type — string, number, list, `none`:

```
var r = risky() catch (e){ :: "error: " + str(e) }
```

`catch` works anywhere an expression is valid — inside arguments, assigned to variables, nested, or chained:

```
# Catch inside an argument
print(safe_div(1, 0) catch (e){ :: 0 })

# Chained catch: re-throw from a handler is caught by the next catch
always_fails() catch (e){ !! "re-thrown" } catch (e2){ :: "got: " + e2 }

# Nested: outer error falls back to an inner computation that may itself fail
safe_div(10, 0) catch (e){
    :: safe_div(6, 2) catch (e2){ :: "all failed" }
}
```

#### Unhandled errors

If an error reaches the top level without being caught, the interpreter prints an error message and exits:

```
[Error] Unhandled error: division by zero
```

In the REPL, unhandled errors print the message and return to the prompt, preserving all state.

---

### Modules

Use `how` to import a `.how` file as a module. The module is bound under
a name, and all its exports are also bound directly into the calling scope.

```
how lru_cache               # binds module as "lru_cache"
how graph as g              # binds module as "g"
how samples/lru_cache as c  # path with slashes — no quotes needed
how "path/to/mod" as m      # quoted path also accepted

where samples               # add directory to search path (no quotes needed)
how graph                   # now finds samples/graph.how
```

After `how graph as g`, use `g.graph[]` to access the class inside the module.
The `where` directive and path imports work with both relative and absolute paths.

### Operators

| Category   | Operators                               |
|------------|-----------------------------------------|
| Arithmetic | `+` `-` `*` `/` `%`                    |
| List concat| `+` (when both sides are lists)         |
| Comparison | `==` `!=` `<` `>` `<=` `>=`            |
| Logical    | `and` `or` `not`  (also `&&` `\|\|` `!`) |
| Augmented  | `+=` `-=` `*=` `/=` `%=`               |
| Error      | `!!` (throw)  `catch` (recover)        |

String concatenation uses `+` (auto-coerces either side to string).

---

## Built-in Functions

| Name                    | Description                                              |
|-------------------------|----------------------------------------------------------|
| `print(x, ...)`         | Print one or more values                                 |
| `len(x)`                | Length of list, map, or string                           |
| `range(n)`              | List `[0..n-1]`; also `range(a,b)`, `range(a,b,step)`   |
| `str(x)`                | Convert to string                                        |
| `num(x)`                | Convert to number                                        |
| `bool(x)`               | Convert to bool                                          |
| `type(x)`               | Type name as string                                      |
| `list()`                | Create an empty mutable list                             |
| `map()`                 | Create an empty mutable map                              |
| `push(lst, v)`          | Append value to list (mutates in place)                  |
| `pop(lst)`              | Remove and return last element                           |
| `keys(m)`               | List of keys in a map or instance                        |
| `values(m)`             | List of values in a map or instance                      |
| `has_key(m, k)`         | True if key exists (`m(k) != none` is equivalent)        |
| `get_key(m, k)`         | Get by key/index (`m(k)` is equivalent)                  |
| `set_key(m, k, v)`      | Set by key/index (`m(k) = v` is equivalent)              |
| `del_key(m, k)`         | Delete a key from a map (no call-syntax equivalent)      |
| `abs(n)`                | Absolute value                                           |
| `floor(n)`              | Floor                                                    |
| `ceil(n)`               | Ceiling                                                  |
| `sqrt(n)`               | Square root                                              |
| `max(...)` / `min(...)` | Max/min of args or a single list                         |
| `ask(prompt)`           | Print prompt and read a line from stdin                  |
| `read(path)`            | Read entire file as a string                             |
| `write(path, v)`        | Write value to file (strings written raw, others repr'd) |
| `args()`                | List of command-line arguments                           |
| `gc()`                  | Trigger a garbage collection cycle                       |
| `cwd()`                 | Return the current working directory as a string         |
| `run(cmd)`             | Execute a shell command; returns the exit code as a number |

---

## REPL

Running `./howlang` with no arguments starts the interactive REPL:

```
Howlang  |  Ctrl-D or quit() to exit
>> var x = 6 * 7
>> x
42
>> x + 1
43
>> _
43
```

**Features:**
- **Auto-print** — bare expressions print their value automatically; statements (`var`, `how`, assignments) do not
- **`_`** — holds the last expression result (like Python's `_`)
- **↑ / ↓** — browse command history (up to 500 entries)
- **← / →**, **Ctrl-A / E** — cursor movement within the line
- **Ctrl-K** — delete to end of line; **Ctrl-U** — clear the line
- **Graceful errors** — parse and runtime errors print a message and the REPL continues; state is preserved
- **Source context** — errors show the offending line and a `^` caret pointing at the problem

---

## Language Design Notes

### Everything is a function
- A **Map** `{"a": 1}` is a function from key → value
- A **List** `{1,2,3}` is a function from index → value  
- A **Class** `[x,y]{...}` is a function that returns an instance map
- A **Branch block** `(a,b){...}` is the general case

### Loop semantics summary

| Syntax | Meaning |
|--------|---------|
| `(:){ ... }()` | Unbounded loop, `::` breaks and returns |
| `(:)= { ... }` | Same but auto-executes; `break` exits without return |
| `(i=a:b){ ... }()` | For-range loop; `::` exits immediately with a value |

### Branch firing rules

`:` and `::` branches behave the same everywhere — in functions, loops, and for-range loops. The rules are simple:

- **Unconditional branches** (no condition, or `var` declarations) — always run
- **`:` side-effect branches** — **all** matching branches fire independently, same as in a function body
- **`::` return/exit branches** — the first truthy one fires and exits immediately
- **`!!` throw branches** — the first truthy one raises an error and unwinds the stack
- **`continue`** — skips remaining branches for this iteration, advances to next
- **`break`** — exits the loop entirely

This means `:` is **"if"** and `::` is **"if/else-if/return"**. They compose cleanly:

```
var x = 0
(:){
    x > 1: print("x is " + str(x)),   # fires at x=2 AND x=3 (independent checks)
    x >= 3 :: x,                        # exits at x=3
    x += 1
}()
# prints "x is 2" and "x is 3", returns 3
```

When you want mutually exclusive choices, use `::` — it stops at the first match:

```
# FizzBuzz using :: for if/else-if semantics
var i = 1
(:){
    i > 20 :: none,
    (){
        i % 15 == 0 :: print("FizzBuzz"),
        i % 3  == 0 :: print("Fizz"),
        i % 5  == 0 :: print("Buzz"),
        :: print(str(i))
    }(),
    i += 1
}()
```

Loops are also valid **statements** inside functions — a loop followed by `:: value` correctly returns after the loop completes:

```
var sum_to = (n){
    var acc = 0
    (i=0:n){ acc += i }()   # loop runs as a statement
    :: acc                   # fires after the loop
}
sum_to(5)   # 10
```

---

## Error Messages

Both parse errors and runtime errors show the source file, line, and the offending code:

```
[ParseError] expected ')'; expected ')' but got 'var'
  --> script.how:4:1
   4 | var z = 5
     | ^
Hint: this usually means a missing ')' earlier, or a trailing comma.

[RuntimeError] undefined variable 'x'
  --> script.how:2
   2 | var y = x + 1

[RuntimeError] not callable (value is a number)
  --> script.how:5
   5 | result(10)
```

**Parse errors** show file, line, and column with a `^` caret. Context hints explain common mistakes:

| Situation | Hint shown |
|---|---|
| Unexpected `::` outside a function | `::` is only valid inside function and loop bodies |
| Unexpected `:` | `:` is a side-effect branch; use `::` to return a value |
| Unexpected `,` where `)` expected | Missing `)` earlier, or trailing comma |
| Missing `}` or `]` | Suggests the unclosed bracket type |

**Runtime errors** show file and line with the source line for context. Common messages:

| Error | Cause |
|---|---|
| `undefined variable 'x'` | Used before `var x = ...` |
| `not callable (value is a number)` | Called `x(...)` but `x` is not a function/map/list |
| `no key 'k' in map` | Dot-access on a key that doesn't exist (use `m("k")` for safe none-returning lookup) |
| `'*' requires numbers` | Type mismatch in arithmetic |
| `expected N args but got M` | Wrong number of arguments to a function |
| `assignment to undeclared variable 'x'` | Assigned without `var x = ...` first |

---

## File Structure

```
howlang/
  c_interpreter/              # C interpreter (CMake project)
    CMakeLists.txt
    include/
      common.h                # Shared utilities: buffers, memory, error reporting, REPL state
      ast.h                   # AST node types and structures
      frontend.h              # Public parser API
      runtime.h               # Public runtime API
      lexer_internal.h        # Token types shared between lexer and parser (internal)
      runtime_internal.h      # Types, globals, and declarations shared across runtime, gc, builtins (internal)
    src/
      common.c                # Implementations of common utilities
      lexer.c                 # Tokeniser
      parser.c                # Parser and AST construction
      frontend.c              # AST list helpers (nl_push, sl_push, make_node, …)
      gc.c                    # GC state, value/env/map/list constructors, mark-sweep collector
      builtins.c              # Built-in functions and global environment setup
      runtime.c               # Evaluator, module import, and public bootstrap/run API
      driver.c                # CLI entry point and REPL
    build/                    # CMake build output (gitignored)
  samples/
    how_interpreter/
      how_meta.how            # Meta-interpreter entry point
      how_lexer.how           # Lexer written in Howlang
      how_parser.how          # Parser written in Howlang
      how_eval.how            # Evaluator written in Howlang
    test_all.how              # Main test suite
    lru_cache.how             # LRU cache module
    lru_cache_test.how        # LRU cache tests
    graph.how                 # Graph + Dijkstra module
    graph_test.how            # Graph tests
    test_loops.how            # Loop semantics tests
    try_catch_test.how        # Error handling tests
```

---

## Running the Tests

```bash
cd c_interpreter
cmake -S . -B build && cmake --build build
HOW=./build/howlang

$HOW ../samples/test_all.how
# FINAL: 54/54 passed

$HOW ../samples/how_interpreter/how_meta.how ../samples/test_all.how
# FINAL: 54/54 passed  (same suite via the meta-interpreter)

cd ../samples
../c_interpreter/build/howlang lru_cache_test.how
# 34 passed   0 failed

../c_interpreter/build/howlang graph_test.how
# 32 passed   0 failed

../c_interpreter/build/howlang test_loops.how
# 41 passed   0 failed

../c_interpreter/build/howlang try_catch_test.how
# 28 passed   0 failed
```
