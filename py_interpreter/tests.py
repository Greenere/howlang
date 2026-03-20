#!/usr/bin/env python3
"""
Test suite for Howlang. Runs all examples from the language spec.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

from lexer import Lexer, LexError
from parser import Parser, ParseError
from interpreter import Interpreter, HowError, HowReturn, how_repr

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

results = []

def test(name: str, source: str, expected=None, expect_output=None, should_error=False):
    interp = Interpreter()
    captured = []
    # Monkey-patch print to capture output
    orig_print = interp.globals.vars["print"]
    def mock_print(*args):
        captured.append(" ".join(how_repr(a) for a in args))
        return None
    interp.globals.vars["print"] = mock_print

    try:
        from lexer import Lexer
        from parser import Parser
        tokens = Lexer(source).tokenize()
        ast = Parser(tokens).parse()
        interp.run(ast)

        if should_error:
            results.append((FAIL, name, "Expected an error but none was raised"))
            return

        if expect_output is not None:
            got = "\n".join(captured)
            if got == expect_output:
                results.append((PASS, name, ""))
            else:
                results.append((FAIL, name, f"Output mismatch:\n  expected: {expect_output!r}\n  got:      {got!r}"))
            return

        results.append((PASS, name, ""))

    except (LexError, ParseError, HowError, HowReturn) as e:
        if should_error:
            results.append((PASS, name, f"(error as expected: {e})"))
        else:
            results.append((FAIL, name, str(e)))
    except Exception as e:
        if should_error:
            results.append((PASS, name, f"(error as expected: {e})"))
        else:
            results.append((FAIL, name, f"Unexpected exception: {type(e).__name__}: {e}"))

# ─── Variables & Primitives ──────────────────────────────────────────────────

test("var number", "var a = 2")
test("var string", 'var d = "Kelly"')
test("var bool",   "var e = true")
test("var none",   "var f = none")

test("var map", '''
var b = {
    "name": "Kelly",
    "age": 21
}
''')

test("var list", "var c = {1,2,3,4}")

# ─── Map access ──────────────────────────────────────────────────────────────

test("map access", '''
var b = {"name": "Kelly", "age": 21}
print(b("name"))
''', expect_output="Kelly")

test("list access", '''
var c = {1,2,3,4}
print(c(0))
''', expect_output="1")

# ─── Arithmetic ──────────────────────────────────────────────────────────────

test("addition",        "var x = 1 + 2",)
test("subtraction",     "var x = 5 - 3")
test("multiplication",  "var x = 4 * 3")
test("division",        "var x = 10 / 2")
test("modulo",          "var x = 7 % 3")

test("arithmetic result", '''
var x = 3 + 4 * 2
print(x)
''', expect_output="11")

test("string concat", '''
var s = "Hello, " + "world!"
print(s)
''', expect_output="Hello, world!")

# ─── Function definition & call ──────────────────────────────────────────────

test("simple function sum", '''
var sum = (a,b){
    :: a + b
}
print(sum(3, 4))
''', expect_output="7")

test("branching function max_two", '''
var max_two = (a, b){
    a > b :: a,
    :: b
}
print(max_two(3, 7))
print(max_two(9, 2))
''', expect_output="7\n9")

# ─── Recursive function ──────────────────────────────────────────────────────

test("recursive sum", '''
var sum = (nums){
    len(nums) == 1 :: nums(0),
    :: nums(0) + sum(nums(1:))
}
var nums = {1,2,3,4,5}
print(sum(nums))
''', expect_output="15")

test("recursive max", '''
var max_two = (a, b){
    a > b :: a,
    :: b
}
var max = (nums){
    var n = len(nums),
    n == 1 :: nums(0),
    :: max_two(nums(0), max(nums(1:)))
}
print(max({3,1,4,1,5,9,2,6}))
''', expect_output="9")

# ─── Slicing ─────────────────────────────────────────────────────────────────

test("slice from index", '''
var c = {10,20,30,40}
var s = c(1:)
print(s(0))
print(s(1))
''', expect_output="20\n30")

test("slice range", '''
var c = {10,20,30,40,50}
var s = c(1:3)
print(len(s))
print(s(0))
''', expect_output="2\n20")

# ─── Branch (if-else analog) ─────────────────────────────────────────────────

test("anonymous branch call", '''
(){
    2 > 1 :: print("yes"),
    :: print("no")
}()
''', expect_output="yes")

test("branch with no match falls to default", '''
(){
    1 > 2 :: print("no"),
    :: print("default")
}()
''', expect_output="default")

# ─── Unbounded loop ───────────────────────────────────────────────────────────

test("basic loop with break", '''
var a = 0
(:){
    a > 4 :: print("done"),
    a += 1
}()
''', expect_output="done")

test("loop sum", '''
var nums = {1,2,3,4,5}
var res = 0
var i = 0
(:){
    i >= len(nums) :: res,
    res += nums(i),
    i += 1
}()
print(res)
''', expect_output="15")

test("loop sum returned", '''
var nums = {1,2,3,4,5}
var i = 0
var res = 0
var total = (:){
    i >= len(nums) :: res,
    res += nums(i),
    i += 1
}()
print(total)
''', expect_output="15")

# ─── Local variables in functions ─────────────────────────────────────────────

test("local var in function", '''
var add_with_local = (a, b){
    var tmp = a + b,
    :: tmp
}
print(add_with_local(10, 5))
''', expect_output="15")

# ─── Class ────────────────────────────────────────────────────────────────────

test("class definition and method call", '''
var person = [name, age]{
    "name": name,
    "age": age,
    "greet": (){
        :: print("Hi, I am " + name)
    }
}
var p = person["Green", 21]
p.greet()
''', expect_output="Hi, I am Green")

test("class field access", '''
var point = [x, y]{
    "x": x,
    "y": y
}
var p = point[3, 4]
print(p.x)
print(p.y)
''', expect_output="3\n4")

# ─── Comparison & logic ───────────────────────────────────────────────────────

test("not operator", '''
var x = not true
print(x)
''', expect_output="false")

test("and operator", '''
print(true and false)
print(true and true)
''', expect_output="false\ntrue")

test("or operator", '''
print(false or true)
print(false or false)
''', expect_output="true\nfalse")

# ─── Augmented assignment ─────────────────────────────────────────────────────

test("augmented assignment", '''
var x = 10
x += 5
x -= 2
x *= 3
print(x)
''', expect_output="39")

# ─── String features ──────────────────────────────────────────────────────────

test("str() builtin", '''
var n = 42
print(str(n))
''', expect_output="42")

test("num() builtin", '''
var s = "3.14"
print(num(s))
''', expect_output="3.14")

# ─── Builtins ────────────────────────────────────────────────────────────────

test("len builtin", '''
var c = {1,2,3}
print(len(c))
''', expect_output="3")

test("range builtin", '''
var r = range(3)
print(r(0))
print(r(2))
''', expect_output="0\n2")

test("type builtin", '''
print(type(1))
print(type("hi"))
print(type(true))
print(type(none))
''', expect_output="number\nstring\nbool\nnone")

# ─── Fibonacci (combining recursion + branching) ──────────────────────────────

test("fibonacci recursive", '''
var fib = (n){
    n <= 1 :: n,
    :: fib(n-1) + fib(n-2)
}
print(fib(0))
print(fib(1))
print(fib(10))
''', expect_output="0\n1\n55")

# ─── min using loop ──────────────────────────────────────────────────────────

test("min using loop", '''
var min = (nums){
    len(nums) == 1 :: nums(0),
    var cur = nums(0),
    var i = 1,
    :: (:){
        i > len(nums) - 1 :: cur,
        cur > nums(i): {
            cur = nums(i)
        },
        i = i + 1
    }()
}
print(min({5,3,8,1,9,2}))
''', expect_output="1")

# ─── Error cases ──────────────────────────────────────────────────────────────

test("undefined variable", "print(xyz)", should_error=True)
test("wrong arg count", '''
var f = (a,b){ :: a+b }
f(1)
''', should_error=True)

# ─── New feature tests ───────────────────────────────────────────────────────

# Semicolons as optional terminators
test("semicolons as terminators", '''
var a = 1;
var b = 2;
print(a + b);
''', expect_output="3")

# List concatenation
test("list concatenation", '''
var a = {1,2,3}
var b = {4,5}
var c = a + b
print(len(c))
print(c(0))
print(c(4))
''', expect_output="5\n1\n5")

test("list concat in sort (merge)", '''
var merge = (a, b){
    len(a) == 0 :: b,
    len(b) == 0 :: a,
    a(0) <= b(0) :: a(:1) + merge(a(1:), b),
    :: b(:1) + merge(b(1:), a)
}
var r = merge({1,3,5}, {2,4,6})
print(r(0))
print(r(5))
''', expect_output="1\n6")

# Slice with no start
test("slice with no start", '''
var c = {10,20,30,40}
var s = c(:2)
print(len(s))
print(s(0))
print(s(1))
''', expect_output="2\n10\n20")

test("slice with no start and no stop", '''
var c = {1,2,3,4,5}
var s = c(:)
print(len(s))
''', expect_output="5")

# Single-element list literal
test("single element list literal", '''
var x = {42}
print(len(x))
print(x(0))
''', expect_output="1\n42")

# Class with bare identifier field names
test("class with bare identifier keys", '''
var point = [x, y]{
    x: x,
    y: y
}
var p = point[10, 20]
print(p.x)
print(p.y)
''', expect_output="10\n20")

test("class method using bare identifier keys", '''
var person = [name, age]{
    name: name,
    age: age,
    greet: (phrase){
        :: print("Hi " + phrase + " I am " + name)
    }
}
var p = person["Alice", 30]
print(p.name)
p.greet("there")
''', expect_output="Alice\nHi there I am Alice")

# For-range loop
test("for-range loop basic", '''
var sum = 0
(0:5) = i{
    sum += i
}
print(sum)
''', expect_output="10")

test("for-range loop max", '''
var max_fn = (nums){
    len(nums) == 1 :: nums(0),
    var cur = nums(0),
    :: (1:len(nums)) = i{
        cur < nums(i): cur = nums(i),
        :: cur
    }
}
print(max_fn({3,1,9,2,7}))
''', expect_output="9")

# Explicit break in (:)= loop
test("break in loop", '''
var i = 0
(:)={
    i > 4: break,
    i += 1
}
print(i)
''', expect_output="5")

# Tree traversal (from tree.how)
test("tree traversal", '''
var node = [val, left, right, leaf]{
    val: val,
    left: left,
    right: right,
    leaf: leaf
}
var root = node[0,
    node[1, none, none, true],
    node[2, none, none, true],
    false]
var traverse = (n){
    n == none :: {},
    n.leaf :: {n.val},
    :: traverse(n.left) + {n.val} + traverse(n.right)
}
var res = traverse(root)
print(res(0))
print(res(1))
print(res(2))
''', expect_output="1\n0\n2")

# Merge sort (from sort.how)
test("merge sort", '''
var merge = (nums1, nums2){
    len(nums1) == 0 :: nums2,
    len(nums2) == 0 :: nums1,
    nums1(0) <= nums2(0) :: nums1(:1) + merge(nums1(1:), nums2),
    :: nums2(:1) + merge(nums2(1:), nums1)
}
var sort_fn = (nums){
    len(nums) == 1 :: {nums(0)},
    len(nums) == 2 :: (ns){
        ns(0) <= ns(1) :: ns,
        :: {ns(1), ns(0)}
    }(nums),
    :: merge(sort_fn(nums(:len(nums)/2)), sort_fn(nums(len(nums)/2:)))
}
var result = sort_fn({5,2,8,1,9,3})
print(result(0))
print(result(5))
''', expect_output="1\n9")

# None equality
test("none equality", '''
var x = none
print(x == none)
print(x != none)
''', expect_output="true\nfalse")


# ─── Report ──────────────────────────────────────────────────────────────────

print()
print("=" * 60)
passed = sum(1 for r in results if r[0] == PASS)
failed = sum(1 for r in results if r[0] == FAIL)
for status, name, detail in results:
    suffix = f"  → {detail}" if detail and status == FAIL else (f"  ({detail})" if detail else "")
    print(f"  {status}  {name}{suffix}")
print("=" * 60)
print(f"  {passed}/{len(results)} tests passed")
print()

if failed > 0:
    sys.exit(1)

