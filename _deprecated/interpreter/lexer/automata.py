_RESERVED_WORDS = {'var'}
_OPERATOR_CHARS = {'.','/','+','-','*','=', ':', '<','>'}
_SEPARATOR_CHARS = {',','\n'}
_BRACKET_CHARS = {'(',')','[',']','{','}'}

_DIGIT = 'digit'
_ALPHABET = 'alphabet'
_SPACE = 'space'
_OPERATOR = 'operator'
_SEPARATOR = 'separator'
_BRACKET = 'bracket'

_TERM_ERROR = -1
_TERM_RESERVED = 0
_TERM_VARIABLE = 1
_TERM_LITERAL = 2
_TERM_OPERATOR = 3
_TERM_SPACE = 4
_TERM_SEPARATOR = 5
_TERM_BRACKET = 6

class Autonode(object):
    def __init__(self, terminal = False, terminal_type = None):
        self.branch = dict()
        self.terminal = terminal
        self.terminal_type = terminal_type
    
    def map_cond(self, char):
        if ord('0') <= ord(char) <= ord('9'):
            return _DIGIT
        if ord('a') <= ord(char) <= ord('z'):
            return _ALPHABET
        if ord('A') <= ord(char) <= ord('Z'):
            return _ALPHABET
        if ord(char) == 32:
            return _SPACE
        if char in _OPERATOR_CHARS:
            return _OPERATOR
        if char in _SEPARATOR_CHARS:
            return _SEPARATOR
        if char in _BRACKET_CHARS:
            return _BRACKET
    
    def add_jump(self, condition, next_node):
        self.branch[condition] = next_node

    def add_terminal_jumps(self, term):
        self.add_jump(_SPACE, term)
        self.add_jump(_SEPARATOR, term)
        self.add_jump(_OPERATOR, term)
        self.add_jump(_BRACKET, term)
    
    def get_next(self, char):
        if char in self.branch:
            return self.branch[char]

        condition = self.map_cond(char)
        if condition in self.branch:
            return self.branch[condition]
        return None

def _add_reserved_word(word, start, fallback, variable, reserved):
    cur = Autonode()
    start.add_jump(word[0], cur)
    cur.add_jump(_ALPHABET, fallback)
    cur.add_jump(_DIGIT, fallback)
    cur.add_terminal_jumps(variable)
    prev = cur 
    for i, w in enumerate(word[1:]):
        cur = Autonode()
        prev.add_jump(w, cur)
        cur.add_jump(_ALPHABET, fallback)
        cur.add_jump(_DIGIT, fallback)
        if i == len(word) - 2:
            cur.add_terminal_jumps(reserved)
        else:
            cur.add_terminal_jumps(variable)
        prev = cur
    

def _build_reserved_automata(start):
    reserved = Autonode(True, _TERM_RESERVED)
    variable = Autonode(True, _TERM_VARIABLE)

    v1 = Autonode()
    v1.add_terminal_jumps(variable)
    v1.add_jump(_ALPHABET, v1)
    v1.add_jump(_DIGIT, v1)
    start.add_jump(_ALPHABET, v1)

    for keyword in _RESERVED_WORDS:
        _add_reserved_word(keyword, start, v1, variable, reserved)

def _build_literal_automata(start):
    literal = Autonode(True, _TERM_LITERAL)
    
    d1 = Autonode()
    d1.add_terminal_jumps(literal)

    d2 = Autonode()
    d2.add_jump(_DIGIT, d2)
    d2.add_terminal_jumps(literal)

    start.add_jump('0', d1)
    start.add_jump(_DIGIT, d2)

    d3 = Autonode()
    d1.add_jump('.', d3)
    d2.add_jump('.', d3)

    d4 = Autonode()
    d4.add_jump(_DIGIT, d4)
    d3.add_jump(_DIGIT, d4)
    d4.add_terminal_jumps(literal)

def _build_single_automata(start):
    operator = Autonode(True, _TERM_OPERATOR)
    space = Autonode(True, _TERM_SPACE)
    separator = Autonode(True, _TERM_SEPARATOR)
    bracket = Autonode(True, _TERM_BRACKET)

    start.add_jump(_OPERATOR, operator)
    start.add_jump(_SEPARATOR, separator)
    start.add_jump(_SPACE, space)
    start.add_jump(_BRACKET, bracket)

def _build_automata():
    start = Autonode()
    _build_single_automata(start)
    _build_literal_automata(start)
    _build_reserved_automata(start)
    return start

def print_tokens(tokens):
    type_map = {
        _TERM_RESERVED: "RESERVED",
        _TERM_SEPARATOR: "SEPARATOR",
        _TERM_VARIABLE: "VARIABLE",
        _TERM_OPERATOR: "OPERATOR",
        _TERM_SPACE: "SPACE",
        _TERM_BRACKET: "BRACKET",
        _TERM_LITERAL: "LITERAL"
    }

    return ''.join([str((r, c, chars, type_map[term])) + "\n" for r, c , chars, term in tokens])


if __name__ == "__main__":
    start = _build_automata()

    tokens = []
    strings = "var abc1 = 2.345\n"
    a, b = 0, 0
    cur = start
    while b < len(strings):
        char = strings[b]
        cur = cur.get_next(char)
        if not cur:
            print("Lexical error at %s"%(strings[a:b+1]))
            break
        if cur.terminal:
            tokens.append([strings[a:b+1], cur.terminal_type])
            cur = start
            a = b + 1
        b += 1
    
    print(tokens)

