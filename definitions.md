# Howlang

Haoyang Li, 20221003

This is a for-fun programming language

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
```

A variable can be declared without type hint, it can be a primitive type:
- Number
- String
- Bool

Or a derivative type:
- Map
- List

## Declare a function

```howlang
var sum = (a,b){
    :: a + b
}
```

A function is a special type of Map that accepts arguments with a default mapping option "::" that denotes the return. The function is by default a branching map, which means that when it is called, only the one with key that evaluates true will be set off.

Local variables:

```howlang
var max_two = (a, b){
    a > b: a,
    :: b
}

var max = (nums){
    n: len(nums), # this is a local variable
    n == 1: nums(0),
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

## Declare a class [TODO]

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
    2 > 1: print("Hi, 2 > 1")
    2 == 1: print("Hi, 2 == 1")
    2 < 1: print("Hi, 2 < 1")
}()
```

There is no "if", a block "{}" is by default a branching map.

## Loop

There is no iteration loop, every loop should be conducted recursively.

Get the sum of a list:

```howlang
var sum = (nums){
    len(nums) == 1: nums(0),
    :: nums(0) + sum(nums(1:))
}
```

