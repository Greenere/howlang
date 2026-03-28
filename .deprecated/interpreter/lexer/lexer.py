from automata import _build_automata, print_tokens

class Lexer(object):
    def __init__(self):
        self.start = _build_automata()
    
    def tokenize(self, string):
        tokens = []
        i, p = 0, 0
        row, col = 0, 0
        cur = self.start
        while i < len(string):
            char = string[i]
            cur = cur.get_next(char)
            if not cur:
                print("Lexical error at line %d column %d: %s"%(row, col, string[p:i+1]))
                break
            if cur.terminal:
                tokens.append((row, col - (i - p), 
                               string[p:i] if p < i else string[p],
                               cur.terminal_type))
                cur = self.start
                if p < i:
                    i -= 1
                    col -= 1
                    p = i + 1
                else:
                    p = i + 1
                    if char == '\n':
                        row += 1
                        col = 0

            i += 1
            col += 1
        return tokens

if __name__ == "__main__":
    lexer = Lexer()
    with open('../../samples/sample.how', 'r') as f:
        string = f.read()
    tokens = lexer.tokenize(string)
    with open('temp/sample.tokens', 'w') as f:
        f.write(str(print_tokens(tokens)))