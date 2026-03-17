# Howlang

Haoyang Li, 20221003

## Declare a variable

```howlang
var a = 2
var b = {
    "name": "Kelly",
    "age": 21
}
var c = {1,2,3,4}
var d = "Kelly"
var e = true
var f = none
```

A variable can be declared without type hint, it can be a primitive type:
- Number
- String
- Bool
- None

Or a derivative type:
- Map
- List

## Declare a function

```howlang
var sum = (a,b){
    :: a + b
}
```

The function is by default a branching map, which means that when it is called, only the one with key that evaluates true will be set off; using "::" means that this branch is going to be returned, by default, there should be an empty branch with "::".

Local variables:

```howlang
var max_two = (a, b){
    a > b :: a,
    :: b
}

var max = (nums){
    var n = len(nums), # this is a local variable
    n == 1 :: nums(0),
    :: max_two(nums(0), max(nums(0:)))
}
```

Map is a special Function, i.e.

```howlang
var b = {
    "name": "Kelly",
    "age": 21
}

var b = (key) {
    key == "name": "Kelly",
    key == "age":21
}

b("name")
```

List is a special Map/Function, i.e.

```howlang
var c = {1,2,3,4}
var c = {
    0: 1,
    1: 2,
    2: 3,
    4: 4
}
var c = (n){
    n == 0: 1,
    n == 1: 2,
    n == 2: 3,
    n == 4: 4
}
c(0)
```

Or, you can also say that Function is a special Map, with a default key ":".

## Declare a class

```howlang
var person = [name, age]{
    name: name,
    age: age,
    greet: (){
        :: print("Hi, I am " + name + "," + "my age is " + age)
    }
}

var p = person["Green", 21]
p.greet()
```

A class is a special type of Map that accepts arguments for initialization. It is a bundle of callable variables and non-callable variables.

## Branch

```howlang
(){
    2 > 1 :: print("Hi, 2 > 1"), # equivalent to `if 2 > 1 return print("Hi, 2 > 1")`
    2 == 1 :: print("Hi, 2 == 1"),
    2 < 1 :: print("Hi, 2 < 1")
}()
```

There is no if-else statement in howlang. You can implement a branching by implement a function without parameter and call it right away as shown above.

## Loop & Local Variables

Loop should be conducted recursively to make it more elegant.

Get the sum of a list:

```howlang
var sum = (nums){
    len(nums) == 1 :: nums(0),
    :: nums(0) + sum(nums(1:))
}
```

But it can also be made explicitly with a loop block.

Sum with unbounded Loop:

```howlang
var sum = (nums){
    var res = 0,
    var i = 0,
    (:) {
        i < len(nums) : (){
            res += i,
            i += 1
        }()
        :: res # :: will break the unbounded loop
    }()
    :: res
}

# An equivalent implementation is shown below
var sum = (nums){
    var res = 0,
    var i = 0,
    :: (:) {
        i < len(nums) : (){
            res += i,
            i += 1
        }()
        :: res # :: will break the unbounded loop and also return the result
    }()
}
```

A more complicated example is shown below

```howlang
var min = (nums){
    len(nums) == 1 :: nums(0),
    var cur = nums(0),
    var i = 1,
    :: (:){
        i > len(nums) - 1 :: cur,
        cur > nums(i): {
            cur = nums(i),
            i = i + 1
        }
    }()
}
```

In short, you should think unbounded loop as a special function that gets executed infinitely until it hits an "::", which is return, i.e.

```
# The following will loop until the condition "a > 10" is met
var a = 0
(:){
    a > 10 :: print("a reached 10"),
    a += 1
}() # the result will be an output "a reached 10"
```

Be careful that "::" within the scope of a loop block will break the loop and return the result following "::".