# How Lang

Haoyang Li, 2021.12.20

[TOC]

## Idea

What if the type of a variable is defined by its position in an expression? 

What if the operation defines the type of variable?

Example:

variables: a, b, c

- a(b,c) , then a is a function
- d = a(), then a is a class and d is an object of class a

## Initiative

- It should be as simple as possible

    - one primitive data type: string
    - two non-primitive data type: list/array, dict/map
    - interpreted

- Operation defines data type

    - Arithmetic:

        - `a + b`, a and b are numbers

        - `a (+) b`, a and b are strings

        - `a [+] b`, a and b are lists

        - `a {+} b`, a and b are dicts
        - similar for other numeric operations

    - Function/class

        - `a(param)`, `a` is a class
            - Do we need the concept of function? The class constructor can always be the default function.
            - Class is a dict

    - Key words

        - `you a`, declares a variable named `a`
            - It comes with default values and constructors
        - `you a = []/{}`, declares a list/dict named `a`
        - `ruo` = `if`, `fou`= `else`
        - `dai ` = `while`

    