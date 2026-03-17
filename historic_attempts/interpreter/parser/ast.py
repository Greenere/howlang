how_add = 0
how_minus = 1
how_mult = 2
how_div = 3

# Abstract syntax tree
class Literal(object):
    def __init__(self, raw, typ):
        self.type = typ
        self.raw = raw

class AST(object):
    def __init__(self, opv = None, left = None, right = None):
        self.operator = opv
        self.left = left
        self.right = right
    
    def evaluate(self):
        if not self.left and not self.right:
            return self.operator

        if self.operator == how_add:
            return self.left.evaluate() + self.right.evaluate()
        
        if self.operator == how_div:
            return self.left.evaluate() / self.right.evaluate()
        
        if self.operator == how_minus:
            return self.left.evaluate() - self.right.evaluate()
        
        if self.operator == how_mult:
            return self.left.evaluate() * self.right.evaluate()

if __name__ == "__main__":
    root = AST(how_add, AST(3), AST(4))
    print(root.evaluate())
        