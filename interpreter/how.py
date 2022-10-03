def read_util(line, start, sep):
    c = start
    res = ""
    while c < len(line):
        if line[c] == sep:
            break
        res += line[c] if line[c] != " " else ""
    return res, c

namespace = {}

while True:
    line = input(">>")
    cur = 0
    while cur < len(line):
        operand, cur = read_util(line, cur, " ")
        if operand == "var": # definition
            operand, cur = read_util(line, cur, "}")
        else: # calling