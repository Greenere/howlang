# Howlang

A small, expressive programming language by Haoyang Li (2022).  
This repository contains a C interpreter (`howlang.c`) and an optional
self-hosting meta-interpreter written in Howlang itself (`how_interpreter/`).

---

## Quick Start

```bash
# Build
cc -O2 -o howlang howlang.c -lm

# Run a file
./howlang program.how

# Interactive REPL
./howlang

# Meta-interpreter REPL (Howlang interpreting Howlang)
./howlang how_interpreter/how_meta.how
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
{42}          # single-element list
len(nums)     # 5

# List concatenation
{1,2} + {3,4}  # {1,2,3,4}
```

**Map** — any-keyed dictionary:

```
var m = {"name": "Alice", "age": 30}
m("name")        # "Alice"
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
| Augmented  | `+=` `-=` `*=` `/=`                    |

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
| `has_key(m, k)`         | True if key exists in map or list index is valid         |
| `get_key(m, k)`         | Get by key/index, returns `none` if out of range         |
| `set_key(m, k, v)`      | Set by key/index (mutates in place)                      |
| `del_key(m, k)`         | Delete a key from a map                                  |
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

## File Structure

```
howlang/
  howlang.c             # Single-file C interpreter — build with: cc -O2 -o howlang howlang.c -lm
  how_interpreter/
    how_meta.how        # Meta-interpreter entry point
    how_lexer.how       # Lexer written in Howlang
    how_parser.how      # Parser written in Howlang
    how_eval.how        # Evaluator written in Howlang
  samples/
    test_all.how        # 54-test suite (direct interpreter)
    lru_cache.how       # LRU cache module
    lru_cache_test.how  # 34-test suite for LRU cache
    graph.how           # Graph + Dijkstra module
    graph_test.how      # 32-test suite for graph
    test_loops.how      # 41-test suite for loop semantics (loop-as-statement, :: exit)
```

---

## Running the Tests

```bash
./howlang samples/test_all.how
# FINAL: 54/54 passed

./howlang how_interpreter/how_meta.how samples/test_all.how
# FINAL: 54/54 passed  (same suite via the meta-interpreter)

cd samples
../howlang lru_cache_test.how
# 34 passed   0 failed

../howlang graph_test.how
# 32 passed   0 failed

../howlang test_loops.how
# 41 passed   0 failed
```
