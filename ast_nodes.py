"""
AST node definitions for Howlang.
"""
from dataclasses import dataclass, field
from typing import List, Optional, Any, Tuple

# ── Expressions ─────────────────────────────────────────────────────────────

@dataclass
class NumberLit:
    value: float

@dataclass
class StringLit:
    value: str

@dataclass
class BoolLit:
    value: bool

@dataclass
class NoneLit:
    pass

@dataclass
class Identifier:
    name: str

@dataclass
class BinOp:
    op: str
    left: Any
    right: Any

@dataclass
class UnaryOp:
    op: str
    operand: Any

@dataclass
class Assign:
    """Re-assignment: a = expr  or  a += expr  etc."""
    target: Any   # Identifier | DotAccess
    op: str       # "=" | "+=" | "-=" | "*=" | "/="
    value: Any

@dataclass
class DotAccess:
    obj: Any
    attr: str

@dataclass
class Call:
    """f(args) or f[args] — both are calls (class instantiation uses [])."""
    callee: Any
    args: List[Any]
    bracket: bool = False  # True when written with [ ]

@dataclass
class Slice:
    """nums(0:) or nums(1:3) or nums(:1)"""
    collection: Any
    start: Optional[Any]
    stop: Optional[Any]

# ── Branches (the core "function body") ────────────────────────────────────

@dataclass
class Branch:
    """cond :: expr   or   cond : expr   or   :: expr"""
    condition: Optional[Any]   # None means default/unconditional
    body: Any                   # expression or block (list of stmts)
    is_return: bool             # True when written with ::

@dataclass
class FuncExpr:
    """(params){ branches }   or   (:){ branches }  for unbounded loop"""
    params: List[str]
    branches: List[Branch]
    is_loop: bool = False

@dataclass
class ForLoop:
    """
    (start:stop) = varname { branches }
    Iterates varname over range [start, stop).
    If start/stop come from a slice of a collection, iterates indices.
    """
    iter_var: str           # the loop variable name
    start: Any              # start expression (or None for 0)
    stop: Any               # stop expression
    branches: List[Branch]

@dataclass
class BreakLoop:
    """break statement inside a (:)= loop"""
    pass

@dataclass
class ClassExpr:
    """[params]{ branches }"""
    params: List[str]
    branches: List[Branch]

@dataclass
class MapLit:
    """{ key: val, ... }  or  { val, ... }  (list literal)"""
    items: List[Tuple[Any, Any]]   # (key, value); key=None for positional list

@dataclass
class Block:
    """{ stmts }  — used as a branch body that does side effects."""
    stmts: List[Any]

# ── Statements ──────────────────────────────────────────────────────────────

@dataclass
class VarDecl:
    name: str
    value: Any

@dataclass
class ExprStmt:
    expr: Any

@dataclass
class Program:
    stmts: List[Any]
