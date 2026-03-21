# Howlang C Interpreter

Single-file C interpreter for Howlang. No dependencies beyond the C standard library.

## Build on macOS

```bash
# With Apple's built-in clang (no install needed):
cc -O2 -o howlang howlang.c -lm

# Or with Homebrew gcc:
gcc-14 -O2 -o howlang howlang.c -lm
```

## Build on Linux

```bash
gcc -O2 -o howlang howlang.c -lm
```

## Run

```bash
# Run a script
./howlang script.how

# Run multiple files concatenated (like the Python interpreter)
./howlang lib.how test.how

# Start the REPL
./howlang
```

## Examples

```bash
# Fibonacci
echo 'var fib = (n){ n <= 1 :: n, :: fib(n-1)+fib(n-2) }
print(fib(10))' | ./howlang /dev/stdin

# Run the included test files
./howlang examples.how
./howlang lru_cache.how lru_cache_test.how
```

## Language quick reference

```
# Variables
var x = 42
var name = "Howlang"
var flag = true
var nothing = none

# Functions  (:: means return)
var add = (a, b){ :: a + b }
var greet = (name){ print("Hello " + name) }

# Branching (condition :: return,  condition : side-effect)
var abs = (n){
    n < 0 :: -n,
    :: n
}

# Unbounded loop
(:){
    i >= 10: break,
    print(str(i)),
    i += 1
}()

# For-range loop
(0:10) = i{
    print(str(i))
}

# Classes  (bracket syntax)
var Point = [x, y]{
    x: x,
    y: y,
    dist: (){
        :: sqrt(x*x + y*y)
    }
}
var p = Point[3, 4]
print(p.dist())   # 5

# Lists and maps
var lst = {1, 2, 3}
var m   = {"key": "value"}
push(lst, 4)
print(lst(0))       # index access
print(m("key"))     # map access
print(lst(1:3))     # slice

# Module import
how lru_cache       # loads lru_cache.how from same directory
var c = lru_cache[10]
c.put("a", 1)
print(c.get("a"))
```
