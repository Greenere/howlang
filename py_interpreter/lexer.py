import re
from enum import Enum, auto
from dataclasses import dataclass
from typing import Optional

class TT(Enum):
    # Literals
    NUMBER    = auto()
    STRING    = auto()
    BOOL      = auto()
    NONE      = auto()
    IDENT     = auto()
    # Keywords
    VAR       = auto()
    BREAK     = auto()
    IMPORT    = auto()   # "how" keyword
    # Punctuation
    LPAREN    = auto()  # (
    RPAREN    = auto()  # )
    LBRACE    = auto()  # {
    RBRACE    = auto()  # }
    LBRACKET  = auto()  # [
    RBRACKET  = auto()  # ]
    COMMA     = auto()  # ,
    DOT       = auto()  # .
    COLON     = auto()  # :
    DCOLON    = auto()  # ::
    SEMICOLON = auto()  # ; (kept as token type but skipped during lex)
    # Operators
    PLUS      = auto()
    MINUS     = auto()
    STAR      = auto()
    SLASH     = auto()
    PERCENT   = auto()
    EQ        = auto()  # =
    PLUSEQ    = auto()  # +=
    MINUSEQ   = auto()  # -=
    STAREQ    = auto()  # *=
    SLASHEQ   = auto()  # /=
    EQEQ      = auto()  # ==
    NEQ       = auto()  # !=
    LT        = auto()  # <
    GT        = auto()  # >
    LTE       = auto()  # <=
    GTE       = auto()  # >=
    AND       = auto()  # and / &&
    OR        = auto()  # or  / ||
    NOT       = auto()  # not / !
    # Special
    EOF       = auto()

@dataclass
class Token:
    type: TT
    value: object
    line: int
    col: int

    def __repr__(self):
        return f"Token({self.type.name}, {self.value!r}, {self.line}:{self.col})"

KEYWORDS = {
    "var":   TT.VAR,
    "true":  TT.BOOL,
    "false": TT.BOOL,
    "none":  TT.NONE,
    "and":   TT.AND,
    "or":    TT.OR,
    "not":   TT.NOT,
    "break": TT.BREAK,
    "how":    TT.IMPORT,
}

class LexError(Exception):
    pass

class Lexer:
    def __init__(self, source: str):
        self.src = source
        self.pos = 0
        self.line = 1
        self.col = 1

    def error(self, msg):
        raise LexError(f"Lex error at {self.line}:{self.col}: {msg}")

    def peek(self, offset=0):
        i = self.pos + offset
        return self.src[i] if i < len(self.src) else "\0"

    def advance(self):
        ch = self.src[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def match(self, ch):
        if self.peek() == ch:
            self.advance()
            return True
        return False

    def skip_whitespace_and_comments(self):
        while self.pos < len(self.src):
            ch = self.peek()
            if ch in " \t\r\n":
                self.advance()
            elif ch == "#":
                while self.pos < len(self.src) and self.peek() != "\n":
                    self.advance()
            else:
                break

    def read_string(self):
        quote = self.advance()
        buf = []
        while self.pos < len(self.src):
            ch = self.peek()
            if ch == "\\":
                self.advance()
                esc = self.advance()
                buf.append({"n": "\n", "t": "\t", "r": "\r", "\\": "\\",
                            '"': '"', "'": "'"}.get(esc, esc))
            elif ch == quote:
                self.advance()
                break
            else:
                buf.append(self.advance())
        else:
            self.error("Unterminated string")
        return "".join(buf)

    def read_number(self):
        buf = []
        while self.peek().isdigit():
            buf.append(self.advance())
        if self.peek() == "." and self.peek(1).isdigit():
            buf.append(self.advance())
            while self.peek().isdigit():
                buf.append(self.advance())
            return float("".join(buf))
        return int("".join(buf))

    def read_ident(self):
        buf = []
        while self.peek().isalnum() or self.peek() == "_":
            buf.append(self.advance())
        return "".join(buf)

    def tokenize(self):
        tokens = []
        while True:
            self.skip_whitespace_and_comments()
            if self.pos >= len(self.src):
                tokens.append(Token(TT.EOF, None, self.line, self.col))
                break
            line, col = self.line, self.col
            ch = self.peek()

            # Numbers
            if ch.isdigit():
                val = self.read_number()
                tokens.append(Token(TT.NUMBER, val, line, col))
                continue

            # Strings
            if ch in ('"', "'"):
                val = self.read_string()
                tokens.append(Token(TT.STRING, val, line, col))
                continue

            # Identifiers / keywords
            if ch.isalpha() or ch == "_":
                name = self.read_ident()
                tt = KEYWORDS.get(name, TT.IDENT)
                if name == "true":   val = True
                elif name == "false": val = False
                elif name == "none":  val = None
                else:                 val = name
                tokens.append(Token(tt, val, line, col))
                continue

            # Semicolons are optional statement terminators — silently skip
            if ch == ";":
                self.advance()
                continue

            self.advance()  # consume ch

            if ch == ":":
                if self.match(":"):
                    tokens.append(Token(TT.DCOLON, "::", line, col))
                else:
                    tokens.append(Token(TT.COLON, ":", line, col))
            elif ch == "=":
                if self.match("="):
                    tokens.append(Token(TT.EQEQ, "==", line, col))
                else:
                    tokens.append(Token(TT.EQ, "=", line, col))
            elif ch == "!":
                if self.match("="):
                    tokens.append(Token(TT.NEQ, "!=", line, col))
                else:
                    tokens.append(Token(TT.NOT, "!", line, col))
            elif ch == "<":
                if self.match("="):
                    tokens.append(Token(TT.LTE, "<=", line, col))
                else:
                    tokens.append(Token(TT.LT, "<", line, col))
            elif ch == ">":
                if self.match("="):
                    tokens.append(Token(TT.GTE, ">=", line, col))
                else:
                    tokens.append(Token(TT.GT, ">", line, col))
            elif ch == "+":
                if self.match("="):
                    tokens.append(Token(TT.PLUSEQ, "+=", line, col))
                else:
                    tokens.append(Token(TT.PLUS, "+", line, col))
            elif ch == "-":
                if self.match("="):
                    tokens.append(Token(TT.MINUSEQ, "-=", line, col))
                else:
                    tokens.append(Token(TT.MINUS, "-", line, col))
            elif ch == "*":
                if self.match("="):
                    tokens.append(Token(TT.STAREQ, "*=", line, col))
                else:
                    tokens.append(Token(TT.STAR, "*", line, col))
            elif ch == "/":
                if self.match("="):
                    tokens.append(Token(TT.SLASHEQ, "/=", line, col))
                else:
                    tokens.append(Token(TT.SLASH, "/", line, col))
            elif ch == "%":
                tokens.append(Token(TT.PERCENT, "%", line, col))
            elif ch == "&":
                if self.match("&"):
                    tokens.append(Token(TT.AND, "&&", line, col))
                else:
                    self.error("Unexpected '&'")
            elif ch == "|":
                if self.match("|"):
                    tokens.append(Token(TT.OR, "||", line, col))
                else:
                    self.error("Unexpected '|'")
            elif ch == "(":
                tokens.append(Token(TT.LPAREN, "(", line, col))
            elif ch == ")":
                tokens.append(Token(TT.RPAREN, ")", line, col))
            elif ch == "{":
                tokens.append(Token(TT.LBRACE, "{", line, col))
            elif ch == "}":
                tokens.append(Token(TT.RBRACE, "}", line, col))
            elif ch == "[":
                tokens.append(Token(TT.LBRACKET, "[", line, col))
            elif ch == "]":
                tokens.append(Token(TT.RBRACKET, "]", line, col))
            elif ch == ",":
                tokens.append(Token(TT.COMMA, ",", line, col))
            elif ch == ".":
                tokens.append(Token(TT.DOT, ".", line, col))
            else:
                self.error(f"Unexpected character: {ch!r}")

        return tokens
