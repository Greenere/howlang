# Howlang

A small, expressive programming language by Haoyang Li (2022).  
This repository contains a complete Python-based tree-walking interpreter.

---

## Quick Start

```bash
python howlang.py program.how   # run a file
python howlang.py               # interactive REPL
python howlang.py -e 'print("Hello!")'  # one-liner
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

#### For-range loop: `(start:stop) = varname { }`

Iterates `varname` over `[start, stop)`. Side-effect branches (`:`) run
each iteration. A `:: expr` branch is the **post-loop return value** —
it runs once after all iterations complete (not on every iteration):

```
var total = 0
(0:5) = i{
    total += i      # runs for i = 0,1,2,3,4
}
# total == 10

# With a return value:
var max_fn = (nums){
    var cur = nums(0),
    :: (1:len(nums)) = i{
        cur < nums(i): cur = nums(i),
        :: cur           # returned once after all iterations
    }
}
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

| Name              | Description                                   |
|-------------------|-----------------------------------------------|
| `print(x, ...)`   | Print one or more values                      |
| `len(x)`          | Length of list, map, or string                |
| `range(n)`        | List `[0..n-1]`; also `range(a,b)`, `range(a,b,step)` |
| `str(x)`          | Convert to string                             |
| `num(x)`          | Convert to number                             |
| `bool(x)`         | Convert to bool                               |
| `type(x)`         | Type name as string                           |
| `list()`          | Create an empty mutable list                  |
| `push(lst, v)`    | Append value to list (mutates in place)       |
| `pop(lst)`        | Remove and return last element                |
| `keys(m)`         | List of keys in a map or class instance       |
| `values(m)`       | List of values in a map or class instance     |
| `abs(n)`          | Absolute value                                |
| `floor(n)`        | Floor                                         |
| `ceil(n)`         | Ceiling                                       |
| `sqrt(n)`         | Square root                                   |
| `max(...)` / `min(...)` | Max/min of args or a single list        |
| `input(prompt)`   | Read a line from stdin                        |

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
| `(a:b) = i { ... }` | For-range loop; `::` in body is post-loop return value |

### `sample.how` — known bugs in that file
Line 25: `p.geet(...)` is a typo (should be `p.greet`), and there is an extra `)`.  
The interpreter reports these clearly with a source pointer.

---

## File Structure

```
howlang/
  lexer.py        # Tokenizer  (adds: ; skipping, break keyword)
  ast_nodes.py    # AST nodes  (adds: ForLoop, BreakLoop)
  parser.py       # Parser     (adds: for-range, (:)=, bare-ident class keys)
  interpreter.py  # Evaluator  (adds: list concat, ForLoop, break, None==None)
  howlang.py      # Entry point with error context display
  examples.how    # Demo program
  tests.py        # 56-test suite
```

---

## Running the Tests

```bash
python tests.py
# 56/56 tests passed
```
