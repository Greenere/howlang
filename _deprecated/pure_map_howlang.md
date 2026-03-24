# Pure-Map Howlang

Haoyang Li, 20230325

This is a specification for a map-based programming language called howlang.

## Type

It has the following primitive types:

- Number
- String
- Boolean
- None

It has two derivative types:

- Map
- Array

## Declaration

Declare a variable:

```
var0: 0 # declare a variable called var0 with value 0, it can be infered that it is of type Number
var1: "hello" # declare a variable called var1 with value "hello", it can be infered that it is of type String
var2: false # declare a variable called var2 with value false, it can be infered that it is of type Boolean
var3: none # declare a variable called var3 with value none, it can be infered that it is of type None

: 0 # There is a special default variable "_", if no variable name is defined, the value is going to be assigned to the default variable "_"
```

As you can see, it is a dynamically typed language, which means that the type of the variable is infered. Everything following by a "#" is comment.

The valid name of the variable must match the following regex:

```
r'^[_a-zA-Z][_a-zA-Z0-9]*$'
```

The value of type Number must match the following regex:

```
r'^-?\d+(?:\.\d+)?$'
```

The value of type String must match the following regex:

```
r"^(['\"])(?:(?!\1).)*\1$"
```

The value of type Boolean must be either "false" or "true.

The value of type None must be "none".

Declare a Map:

```
map_var0 : {
    var1: 0,
    var2: false,
    var3: "hello",
    var4: none
}
var5: map_var0.var1 # It access the variable "var1" defined inside the "map_var0" and assign it to var5
```

As you can see, a map is like a dictionary in Python, it bundles some variables together, you can use "." to access the value of the inner variables.

The valid name of the map must match the following regex:

```
r'^[_a-zA-Z][_a-zA-Z0-9]*$'
```

Declare an array:

```
arr: [0,1,2,3,4]

arr[0] # this evaluates to the 0-th element in the array
```

Declare multiple variables in one line:

```
a, b: 0, 1 # declare variable "a" with value "0" and variable "b" with value "1"

c, d:[0,1,2],{a:0, b:1} # declare an array "c" with value "[0,1,2]", and a map "d" with value "{a:0, b:1}"
```

## Evaluation

Evaluate a variable:

```
var0: 0
var0 # this will evaluate the variable, and by default print it out
```

Evaluate a variable insdie a Map:

```
map_var1: {
    a: 0
    b: -1
}

map_var1.a # this will evaluate the variable "a" defined inside the map
```

A map can also be evaluated like a function in Python, by default it takes one parameter called "key", and it will matches the key with all variables defined inside the map, if there is a match, evaluate the matched variable, otherwise evaluate as "none".

For example:

```
map_var2: {
    c: 0
    d: -1
}

map_var2("c") # this will evaluate as "0", which is equivalent to map_var2.c

var5: "c"
map_var2(var5) # this will also evaluate as "0", the value of var5 is "c", therefore this is equivalent to map_var2("c")
```

With that, you can use any map as a if-else-branch:

```
map_var3: {
    true: print("This is true") # print is a built-in function that will print out the result
    false: print("This is false")
}

map_var3(1 > 0) # Because 1 > 0 evaluates to true, so it will be equivalent to map_var3(true), which will print "This is true"
```

You can also apply a map to an array, which will apply it to every element in the array and evaluate as a new array:

```
addone: {
    _ : _ + 1
}

arr: [0,1,2,3]
new_arr: addone(arr) # the new array will be [0,1,2,3]
```

## Examples

Calculate Fibonaci array:

```
fibonacci: {
    0: 0
    1: 1
    _: fibonacci(_ - 1) + fibonacci(_ - 2)
}

n: 10
result: fibonacci(n)
print(result) # prints 55
```
