"""
Recursive-descent parser for Howlang.
New in this version:
  - Semicolons silently ignored (already stripped by lexer)
  - List concatenation via + (handled in interpreter)
  - Slice with no start:  nums(:3)
  - Single-element list:  {expr}  still parsed as list if no colon follows
  - for-range loop:       (start:stop) = varname { branches }
  - explicit-break loop:  (:)= { ..., break, ... }
  - break keyword inside loops
  - Graceful error recovery: extra ) produces a clear message
"""

from lexer import TT, Token
from ast_nodes import *
from typing import List, Optional

class ParseError(Exception):
    pass

class Parser:
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.pos = 0

    # ── helpers ─────────────────────────────────────────────────────────────

    def peek(self, offset=0) -> Token:
        i = self.pos + offset
        if i >= len(self.tokens):
            return self.tokens[-1]
        return self.tokens[i]

    def check(self, *types) -> bool:
        return self.peek().type in types

    def advance(self) -> Token:
        t = self.tokens[self.pos]
        if t.type != TT.EOF:
            self.pos += 1
        return t

    def expect(self, tt, msg=None) -> Token:
        t = self.peek()
        if t.type != tt:
            raise ParseError(msg or
                f"Expected {tt.name} but got {t.type.name} ({t.value!r}) at {t.line}:{t.col}")
        return self.advance()

    def match(self, *types) -> Optional[Token]:
        if self.peek().type in types:
            return self.advance()
        return None

    def error(self, msg=""):
        t = self.peek()
        raise ParseError(f"Parse error at {t.line}:{t.col}: {msg} (got {t.type.name} {t.value!r})")

    # ── top level ────────────────────────────────────────────────────────────

    def parse(self) -> Program:
        stmts = []
        while not self.check(TT.EOF):
            stmts.append(self.stmt())
        return Program(stmts)

    def stmt(self):
        if self.check(TT.VAR):
            return self.var_decl()
        return self.expr_stmt()

    def var_decl(self) -> VarDecl:
        self.expect(TT.VAR)
        name = self.expect(TT.IDENT).value
        self.expect(TT.EQ)
        val = self.expr()
        return VarDecl(name, val)

    def expr_stmt(self) -> ExprStmt:
        e = self.expr()
        return ExprStmt(e)

    # ── expressions ──────────────────────────────────────────────────────────

    def expr(self):
        return self.assign()

    def assign(self):
        left = self.or_expr()
        op_tok = self.match(TT.EQ, TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ)
        if op_tok:
            right = self.expr()
            return Assign(left, op_tok.value, right)
        return left

    def or_expr(self):
        left = self.and_expr()
        while self.check(TT.OR):
            op = self.advance().value
            right = self.and_expr()
            left = BinOp(op, left, right)
        return left

    def and_expr(self):
        left = self.not_expr()
        while self.check(TT.AND):
            op = self.advance().value
            right = self.not_expr()
            left = BinOp(op, left, right)
        return left

    def not_expr(self):
        if self.check(TT.NOT):
            op = self.advance().value
            return UnaryOp(op, self.not_expr())
        return self.compare()

    def compare(self):
        left = self.add()
        while self.check(TT.EQEQ, TT.NEQ, TT.LT, TT.GT, TT.LTE, TT.GTE):
            op = self.advance().value
            right = self.add()
            left = BinOp(op, left, right)
        return left

    def add(self):
        left = self.mul()
        while self.check(TT.PLUS, TT.MINUS):
            op = self.advance().value
            right = self.mul()
            left = BinOp(op, left, right)
        return left

    def mul(self):
        left = self.unary()
        while self.check(TT.STAR, TT.SLASH, TT.PERCENT):
            op = self.advance().value
            right = self.unary()
            left = BinOp(op, left, right)
        return left

    def unary(self):
        if self.check(TT.MINUS):
            op = self.advance().value
            return UnaryOp(op, self.unary())
        return self.call_or_access()

    def call_or_access(self):
        node = self.atom()
        last_line = self.tokens[self.pos - 1].line
        while True:
            next_tok = self.peek()
            same_line = (next_tok.line == last_line)
            callable_node = isinstance(node, (Identifier, Call, DotAccess))
            if self.check(TT.LPAREN) and (same_line or callable_node):
                args, is_slice, start, stop = self.call_args()
                if is_slice:
                    node = Slice(node, start, stop)
                else:
                    node = Call(node, args)
                last_line = self.tokens[self.pos - 1].line
            elif self.check(TT.LBRACKET) and (same_line or callable_node):
                self.advance()
                args = self.arg_list(TT.RBRACKET)
                self.expect(TT.RBRACKET)
                node = Call(node, args, bracket=True)
                last_line = self.tokens[self.pos - 1].line
            elif self.check(TT.DOT):
                self.advance()
                attr = self.expect(TT.IDENT).value
                node = DotAccess(node, attr)
                last_line = self.tokens[self.pos - 1].line
            else:
                break
        return node

    def call_args(self):
        """Returns (args, is_slice, start, stop)."""
        self.expect(TT.LPAREN)
        if self.check(TT.RPAREN):
            self.advance()
            return [], False, None, None

        # Detect slice: (:stop) or (start:) or (start:stop)
        start = None
        if not self.check(TT.COLON):
            start = self.expr()
        if self.check(TT.COLON):
            self.advance()
            stop = None
            if not self.check(TT.RPAREN):
                stop = self.expr()
            self.expect(TT.RPAREN)
            return [], True, start, stop

        # Regular call
        args = [start]
        while self.match(TT.COMMA):
            if self.check(TT.RPAREN):
                break
            args.append(self.expr())
        self.expect(TT.RPAREN)
        return args, False, None, None

    def arg_list(self, closing: TT) -> List:
        args = []
        if self.check(closing):
            return args
        args.append(self.expr())
        while self.match(TT.COMMA):
            if self.check(closing):
                break
            args.append(self.expr())
        return args

    def atom(self):
        t = self.peek()

        if t.type == TT.NUMBER:
            self.advance()
            return NumberLit(t.value)

        if t.type == TT.STRING:
            self.advance()
            return StringLit(t.value)

        if t.type == TT.BOOL:
            self.advance()
            return BoolLit(t.value)

        if t.type == TT.NONE:
            self.advance()
            return NoneLit()

        if t.type == TT.IDENT:
            self.advance()
            return Identifier(t.value)

        if t.type == TT.BREAK:
            self.advance()
            return BreakLoop()

        # ( ... ) — function, loop, for-range loop, or grouped expression
        if t.type == TT.LPAREN:
            return self.func_or_group()

        # [ params ] { body } — class definition
        if t.type == TT.LBRACKET:
            return self.class_expr()

        # { ... } — map / list literal or anonymous branch-function
        if t.type == TT.LBRACE:
            return self.map_or_func_literal()

        self.error("Unexpected token in expression")

    def func_or_group(self):
        """
        Handles all forms starting with '(':
          (:){ body }             — unbounded loop
          (:)= { body }           — unbounded loop with break keyword
          (start:stop) = var { }  — for-range loop
          (params){ body }        — function
          (expr)                  — grouped expression
        """
        self.expect(TT.LPAREN)

        # ── Unbounded loop: (:){ }  or  (:)= { } ─────────────────────────
        if self.check(TT.COLON) and self.peek(1).type == TT.RPAREN:
            self.advance()          # consume ':'
            self.expect(TT.RPAREN)
            auto_call = bool(self.match(TT.EQ))   # (:)= means execute immediately
            body = self.func_body()
            fn = FuncExpr([], body, is_loop=True)
            if auto_call:
                return Call(fn, [])   # auto-execute: (:)= { } runs right away
            return fn

        # ── For-range loop: (start:stop) = varname { } ────────────────────
        # Detect: we have a slice-like thing followed by ) = IDENT {
        # Peek ahead carefully: try to parse as (expr:expr) = ident {
        saved = self.pos
        try:
            start = None
            if not self.check(TT.COLON):
                start = self.expr()
            if self.check(TT.COLON):
                self.advance()
                stop = None
                if not self.check(TT.RPAREN):
                    stop = self.expr()
                self.expect(TT.RPAREN)
                # Now expect = varname {
                if self.check(TT.EQ):
                    self.advance()
                    iter_var = self.expect(TT.IDENT).value
                    if self.check(TT.LBRACE):
                        body = self.func_body()
                        return ForLoop(iter_var, start, stop, body)
            # Not a for-range loop — roll back
            self.pos = saved
        except ParseError:
            self.pos = saved

        # ── Try function: (ident, ...) { } ───────────────────────────────
        if self.check(TT.RPAREN):
            self.advance()
            if self.check(TT.LBRACE):
                return FuncExpr([], self.func_body(), is_loop=False)
            return NoneLit()

        if self.check(TT.IDENT):
            saved2 = self.pos
            try:
                params = [self.advance().value]
                while self.match(TT.COMMA):
                    tok = self.peek()
                    if tok.type != TT.IDENT:
                        raise ParseError("not all params are idents")
                    params.append(self.advance().value)
                self.expect(TT.RPAREN)
                if self.check(TT.LBRACE):
                    return FuncExpr(params, self.func_body(), is_loop=False)
                self.pos = saved2
            except ParseError:
                self.pos = saved2

        # ── Grouped expression: (expr) ────────────────────────────────────
        e = self.expr()
        self.expect(TT.RPAREN)
        return e

    def class_expr(self) -> ClassExpr:
        self.expect(TT.LBRACKET)
        params = []
        if not self.check(TT.RBRACKET):
            params.append(self.expect(TT.IDENT).value)
            while self.match(TT.COMMA):
                params.append(self.expect(TT.IDENT).value)
        self.expect(TT.RBRACKET)
        body = self.func_body()
        return ClassExpr(params, body)

    def func_body(self) -> List[Branch]:
        """{ branch, branch, ... }"""
        self.expect(TT.LBRACE)
        branches = []
        while not self.check(TT.RBRACE) and not self.check(TT.EOF):
            branch = self.branch()
            branches.append(branch)
            self.match(TT.COMMA)
        self.expect(TT.RBRACE)
        return branches

    def branch(self) -> Branch:
        # Local variable declaration
        if self.check(TT.VAR):
            decl = self.var_decl()
            return Branch(None, decl, is_return=False)

        # :: expr  — unconditional return
        if self.check(TT.DCOLON):
            self.advance()
            body = self.expr()
            return Branch(None, body, is_return=True)

        # break — explicit loop break (in (:)= loops)
        if self.check(TT.BREAK):
            self.advance()
            return Branch(None, BreakLoop(), is_return=True)

        # cond :: expr  or  cond : body
        cond = self.expr()

        if self.check(TT.DCOLON):
            self.advance()
            body = self.expr()
            return Branch(cond, body, is_return=True)

        if self.check(TT.COLON):
            self.advance()
            if self.check(TT.LBRACE):
                body = self.block()
            else:
                body = self.expr()
            return Branch(cond, body, is_return=False)

        # Bare expression / assignment — side effect with no condition
        return Branch(None, cond, is_return=False)

    def block(self) -> Block:
        """{ stmt, stmt, ... }"""
        self.expect(TT.LBRACE)
        stmts = []
        while not self.check(TT.RBRACE) and not self.check(TT.EOF):
            if self.check(TT.VAR):
                stmts.append(self.var_decl())
            else:
                stmts.append(ExprStmt(self.expr()))
            self.match(TT.COMMA)
        self.expect(TT.RBRACE)
        return Block(stmts)

    def map_or_func_literal(self):
        """
        {}                   → empty map
        {expr, ...}          → list literal
        {key: val, ...}      → map literal
        {cond :: expr, ...}  → anonymous branch function
        """
        self.expect(TT.LBRACE)
        if self.check(TT.RBRACE):
            self.advance()
            return MapLit([])

        # If starts with ::, it's an anonymous branch function
        if self.check(TT.DCOLON):
            branches = []
            while not self.check(TT.RBRACE) and not self.check(TT.EOF):
                branches.append(self.branch())
                self.match(TT.COMMA)
            self.expect(TT.RBRACE)
            return FuncExpr([], branches, is_loop=False)

        first = self.expr()

        # Map literal: first_key : value, ...
        if self.check(TT.COLON):
            self.advance()
            val = self.expr()
            items = [(first, val)]
            while self.match(TT.COMMA):
                if self.check(TT.RBRACE):
                    break
                k = self.expr()
                self.expect(TT.COLON)
                v = self.expr()
                items.append((k, v))
            self.expect(TT.RBRACE)
            return MapLit(items)

        # Branch function: cond :: expr, ...
        if self.check(TT.DCOLON):
            self.advance()
            body = self.expr()
            branches = [Branch(first, body, is_return=True)]
            while self.match(TT.COMMA):
                if self.check(TT.RBRACE):
                    break
                branches.append(self.branch())
            self.expect(TT.RBRACE)
            return FuncExpr([], branches, is_loop=False)

        # List literal: {expr, expr, ...}  — including single-element {expr}
        items = [(None, first)]
        while self.match(TT.COMMA):
            if self.check(TT.RBRACE):
                break
            items.append((None, self.expr()))
        self.expect(TT.RBRACE)
        return MapLit(items)
