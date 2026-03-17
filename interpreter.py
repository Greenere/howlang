import os
"""
Tree-walking interpreter for Howlang.
New in this version:
  - List + List concatenation
  - ForLoop: (start:stop) = i { ... }
  - BreakLoop: break keyword in (:)= loops
  - HowBreak exception for explicit break
  - Class instance fields accessible via dot (.name, .age, etc.)
  - Proper None == None comparison
  - Float slice indices auto-cast to int
"""
from ast_nodes import *
from typing import Any, Dict, List, Optional
import sys

# ── Control flow signals ─────────────────────────────────────────────────────

class HowReturn(Exception):
    def __init__(self, value):
        self.value = value

class HowBreak(Exception):
    """Raised by 'break' in a (:)= loop."""
    pass

class HowError(Exception):
    pass

# ── Environment ───────────────────────────────────────────────────────────────

class Environment:
    def __init__(self, parent: Optional["Environment"] = None):
        self.vars: Dict[str, Any] = {}
        self.parent = parent

    def get(self, name: str) -> Any:
        if name in self.vars:
            return self.vars[name]
        if self.parent:
            return self.parent.get(name)
        raise HowError(f"Undefined variable: {name!r}")

    def set(self, name: str, value: Any):
        self.vars[name] = value

    def assign(self, name: str, value: Any):
        if name in self.vars:
            self.vars[name] = value
            return
        if self.parent:
            self.parent.assign(name, value)
            return
        raise HowError(f"Assignment to undeclared variable: {name!r}")

# ── Runtime values ────────────────────────────────────────────────────────────

class HowFunction:
    def __init__(self, params, branches, closure, is_loop=False):
        self.params: List[str] = params
        self.branches: List[Branch] = branches
        self.closure: Environment = closure
        self.is_loop: bool = is_loop

    def __repr__(self):
        return f"<function params={self.params}>"


class HowClass:
    def __init__(self, params, branches, closure):
        self.params = params
        self.branches = branches
        self.closure = closure

    def __repr__(self):
        return f"<class params={self.params}>"


class HowInstance:
    def __init__(self, fields: Dict[str, Any]):
        self.fields = fields

    def get(self, name: str) -> Any:
        if name in self.fields:
            return self.fields[name]
        raise HowError(f"No field {name!r} on instance")

    def set(self, name: str, value: Any):
        self.fields[name] = value

    def __repr__(self):
        return "<instance {" + ", ".join(f"{k}: {how_repr(v)}" for k, v in self.fields.items()) + "}>"

    def __eq__(self, other):
        return self is other


class HowMap:
    def __init__(self, items: Dict):
        self.items = items

    def get(self, key) -> Any:
        if key in self.items:
            return self.items[key]
        raise HowError(f"Key {key!r} not found in map")

    def __repr__(self):
        return "{" + ", ".join(f"{k!r}: {how_repr(v)}" for k, v in self.items.items()) + "}"

    def __eq__(self, other):
        if isinstance(other, HowMap):
            return self.items == other.items
        return False


class HowList:
    def __init__(self, items: List[Any]):
        self.items = items

    def get(self, idx) -> Any:
        if not isinstance(idx, (int, float)):
            raise HowError(f"List index must be a number, got {idx!r}")
        i = int(idx)
        if i < 0 or i >= len(self.items):
            raise HowError(f"List index {i} out of range (len={len(self.items)})")
        return self.items[i]

    def slice(self, start, stop):
        s = int(start) if start is not None else 0
        e = int(stop)  if stop  is not None else len(self.items)
        return HowList(self.items[s:e])

    def concat(self, other: "HowList") -> "HowList":
        if not isinstance(other, HowList):
            raise HowError(f"Can only concatenate list with list, not {type(other).__name__}")
        return HowList(self.items + other.items)

    def __repr__(self):
        return "[" + ", ".join(how_repr(v) for v in self.items) + "]"

    def __eq__(self, other):
        if isinstance(other, HowList):
            return self.items == other.items
        return False


class HowModule:
    """
    The result of  how <name>.
    Wraps a freshly-executed Howlang environment, exposing its top-level
    variables as attributes:  module.some_var  or  module.some_fn(args).
    """
    def __init__(self, name: str, env: "Environment"):
        self._name = name
        self._env  = env

    def get_attr(self, attr: str) -> Any:
        try:
            return self._env.get(attr)
        except HowError:
            raise HowError(f"Module {self._name!r} has no export {attr!r}")

    def __repr__(self):
        public = [k for k in self._env.vars if not k.startswith("_")]
        return f"<module:{self._name} exports={public}>"


def how_repr(v) -> str:
    if v is None:      return "none"
    if v is True:      return "true"
    if v is False:     return "false"
    if isinstance(v, float):
        return str(int(v)) if v == int(v) else str(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return v
    if isinstance(v, HowModule):
        return repr(v)
    return repr(v)


# ── Interpreter ───────────────────────────────────────────────────────────────

class InstanceEnv(Environment):
    """
    An Environment whose variables are backed by a HowInstance's fields dict.
    Reads and writes to any name that matches a field key go directly to the
    instance, so that method closures see live field values and mutations are
    reflected back on the instance immediately.
    """
    def __init__(self, instance: "HowInstance", parent: "Environment"):
        super().__init__(parent)
        self._inst = instance

    def get(self, name: str) -> Any:
        # Check local vars first (method params, local var decls)
        if name in self.vars:
            return self.vars[name]
        # Then instance fields
        if name in self._inst.fields:
            return self._inst.fields[name]
        # Then parent chain (init params: cap, h, t, ...)
        if self.parent:
            return self.parent.get(name)
        raise HowError(f"Undefined variable: {name!r}")

    def assign(self, name: str, value: Any):
        # Local var takes priority
        if name in self.vars:
            self.vars[name] = value
            return
        # Instance field
        if name in self._inst.fields:
            self._inst.fields[name] = value
            return
        # Parent chain
        if self.parent:
            self.parent.assign(name, value)
            return
        raise HowError(f"Assignment to undeclared variable: {name!r}")


class Interpreter:
    def __init__(self):
        self.globals = Environment()
        self._import_dirs: list = [os.getcwd()]
        self._builtin_names: set = set()
        self._setup_builtins()

    def _setup_builtins(self):
        g = self.globals

        def _print(*args):
            # Filter out None values that come from void calls (e.g. print inside print)
            parts = [how_repr(a) for a in args if a is not None or len(args) == 1]
            print(*parts)
            return None

        def _len(x):
            if isinstance(x, HowList):  return float(len(x.items))
            if isinstance(x, HowMap):   return float(len(x.items))
            if isinstance(x, str):      return float(len(x))
            raise HowError(f"len() not supported for {type(x).__name__}")

        def _range(*args):
            iargs = [int(a) for a in args]
            if len(iargs) == 1:   return HowList([float(i) for i in range(iargs[0])])
            if len(iargs) == 2:   return HowList([float(i) for i in range(iargs[0], iargs[1])])
            if len(iargs) == 3:   return HowList([float(i) for i in range(iargs[0], iargs[1], iargs[2])])
            raise HowError("range() takes 1-3 arguments")

        def _str(x):    return how_repr(x)
        def _num(x):
            try:    return float(x)
            except: raise HowError(f"Cannot convert {x!r} to number")

        def _bool_fn(x): return how_truthy(x)

        def _type(x):
            if x is None:                return "none"
            if isinstance(x, bool):      return "bool"
            if isinstance(x, (int,float)):return "number"
            if isinstance(x, str):       return "string"
            if isinstance(x, HowList):   return "list"
            if isinstance(x, HowMap):    return "map"
            if isinstance(x, HowInstance):return "instance"
            if isinstance(x, HowFunction):return "function"
            if isinstance(x, HowClass):  return "class"
            return "unknown"

        def _list(*args): return HowList(list(args))
        def _map():       return HowMap({})

        def _push(lst, val):
            if not isinstance(lst, HowList):
                raise HowError("push() requires a list")
            lst.items.append(val)
            return lst

        def _pop(lst):
            if not isinstance(lst, HowList):
                raise HowError("pop() requires a list")
            if not lst.items:
                raise HowError("pop() on empty list")
            return lst.items.pop()

        def _keys(m):
            if isinstance(m, HowMap):      return HowList(list(m.items.keys()))
            if isinstance(m, HowInstance): return HowList(list(m.fields.keys()))
            raise HowError("keys() requires a map or instance")

        def _values(m):
            if isinstance(m, HowMap):      return HowList(list(m.items.values()))
            if isinstance(m, HowInstance): return HowList(list(m.fields.values()))
            raise HowError("values() requires a map or instance")

        def _input(prompt=""):  return input(how_repr(prompt))

        def _abs(x):
            if isinstance(x, (int, float)): return float(abs(x))
            raise HowError("abs() requires a number")

        def _floor(x):
            import math; return float(math.floor(x))

        def _ceil(x):
            import math; return float(math.ceil(x))

        def _sqrt(x):
            import math; return float(math.sqrt(x))

        def _max_fn(*args):
            items = args[0].items if len(args) == 1 and isinstance(args[0], HowList) else args
            return max(items, key=lambda v: v if isinstance(v, (int,float)) else 0)

        def _min_fn(*args):
            items = args[0].items if len(args) == 1 and isinstance(args[0], HowList) else args
            return min(items, key=lambda v: v if isinstance(v, (int,float)) else 0)

        def _has_key(m, k):
            if isinstance(m, HowMap):      return k in m.items
            if isinstance(m, HowInstance): return k in m.fields
            if isinstance(m, HowList):     return 0 <= int(k) < len(m.items)
            raise HowError("has_key() requires a map, instance, or list")

        def _set_key(m, k, v):
            if isinstance(m, HowMap):
                m.items[k] = v
                return v
            if isinstance(m, HowInstance):
                m.fields[k] = v
                return v
            if isinstance(m, HowList):
                i = int(k)
                if i < 0 or i >= len(m.items):
                    raise HowError(f"List index {i} out of range (len={len(m.items)})")
                m.items[i] = v
                return v
            raise HowError("set_key() requires a map, instance, or list")

        def _del_key(m, k):
            if isinstance(m, HowMap):
                m.items.pop(k, None)
                return None
            if isinstance(m, HowInstance):
                m.fields.pop(k, None)
                return None
            raise HowError("del_key() requires a map or instance")

        def _get_key(m, k):
            if isinstance(m, HowMap):
                return m.items.get(k, None)
            if isinstance(m, HowInstance):
                return m.fields.get(k, None)
            if isinstance(m, HowList):
                i = int(k)
                if 0 <= i < len(m.items):
                    return m.items[i]
                return None
            raise HowError("get_key() requires a map, instance, or list")

        builtins = {
            "print": _print, "len": _len, "range": _range,
            "str": _str, "num": _num, "bool": _bool_fn, "type": _type,
            "list": _list, "map": _map, "push": _push, "pop": _pop,
            "keys": _keys, "values": _values, "input": _input,
            "abs": _abs, "floor": _floor, "ceil": _ceil,
            "sqrt": _sqrt, "max": _max_fn, "min": _min_fn,
            "has_key": _has_key, "set_key": _set_key,
            "del_key": _del_key, "get_key": _get_key,
        }
        for name, fn in builtins.items():
            g.set(name, fn)
        self._builtin_names = set(builtins.keys())

    # ── Entry point ───────────────────────────────────────────────────────────

    def run(self, program: Program):
        for stmt in program.stmts:
            self.exec_stmt(stmt, self.globals)

    # ── Statements ────────────────────────────────────────────────────────────

    def exec_stmt(self, stmt, env: Environment):
        if isinstance(stmt, VarDecl):
            val = self.eval(stmt.value, env)
            env.set(stmt.name, val)
        elif isinstance(stmt, ImportStmt):
            self.exec_import(stmt, env)
        elif isinstance(stmt, ExprStmt):
            self.eval(stmt.expr, env)
        elif isinstance(stmt, Block):
            for s in stmt.stmts:
                self.exec_stmt(s, env)
        else:
            self.eval(stmt, env)

    def exec_import(self, stmt: "ImportStmt", env: Environment):
        """
        how <name>
        Resolve <name>.how relative to the current search path, execute it
        in a fresh child interpreter (sharing no mutable state), then bind
        the resulting module env as a HowModule under <name>.
        """
        module_name = stmt.module
        # Search paths: directories registered on the interpreter + cwd
        candidate = None
        for search_dir in self._import_dirs:
            path = os.path.join(search_dir, module_name + ".how")
            if os.path.isfile(path):
                candidate = path
                break
        if candidate is None:
            raise HowError(
                f"Cannot find module {module_name!r} "
                f"(searched: {self._import_dirs})"
            )
        # Execute the module in a fresh interpreter that inherits builtins
        mod_interp = Interpreter()
        # Pass import dirs so the module can itself import
        mod_interp._import_dirs = self._import_dirs
        try:
            with open(candidate, "r", encoding="utf-8") as f:
                source = f.read()
            from lexer import Lexer as _Lexer
            from parser import Parser as _Parser
            tokens = _Lexer(source).tokenize()
            ast    = _Parser(tokens).parse()
            mod_interp.run(ast)
        except Exception as e:
            raise HowError(f"Error loading module {module_name!r}: {e}")
        # Expose the module's global scope (minus builtins) as a HowModule
        mod_env = Environment()
        for k, v in mod_interp.globals.vars.items():
            if k not in mod_interp._builtin_names:
                mod_env.vars[k] = v
        module = HowModule(module_name, mod_env)
        env.set(module_name, module)

    # ── Expressions ───────────────────────────────────────────────────────────

    def eval(self, node, env: Environment) -> Any:
        if isinstance(node, NumberLit):  return float(node.value)
        if isinstance(node, StringLit):  return node.value
        if isinstance(node, BoolLit):    return node.value
        if isinstance(node, NoneLit):    return None

        if isinstance(node, Identifier):
            return env.get(node.name)

        if isinstance(node, BreakLoop):
            raise HowBreak()

        if isinstance(node, BinOp):
            return self.eval_binop(node, env)

        if isinstance(node, UnaryOp):
            return self.eval_unaryop(node, env)

        if isinstance(node, Assign):
            return self.eval_assign(node, env)

        if isinstance(node, DotAccess):
            obj = self.eval(node.obj, env)
            if isinstance(obj, HowInstance):
                return obj.get(node.attr)
            if isinstance(obj, HowMap):
                return obj.get(node.attr)
            if isinstance(obj, HowModule):
                return obj.get_attr(node.attr)
            raise HowError(f"Cannot access .{node.attr} on {type(obj).__name__}")

        if isinstance(node, Call):
            return self.eval_call(node, env)

        if isinstance(node, Slice):
            col   = self.eval(node.collection, env)
            start = self.eval(node.start, env) if node.start is not None else None
            stop  = self.eval(node.stop,  env) if node.stop  is not None else None
            if isinstance(col, HowList):
                return col.slice(start, stop)
            if isinstance(col, str):
                s = int(start) if start is not None else 0
                e = int(stop)  if stop  is not None else len(col)
                return col[s:e]
            raise HowError(f"Cannot slice {type(col).__name__}")

        if isinstance(node, FuncExpr):
            return HowFunction(node.params, node.branches, env, is_loop=node.is_loop)

        if isinstance(node, ForLoop):
            return self.run_for_loop(node, env)

        if isinstance(node, ClassExpr):
            return HowClass(node.params, node.branches, env)

        if isinstance(node, MapLit):
            return self.eval_map_lit(node, env)

        if isinstance(node, Block):
            child = Environment(env)
            for s in node.stmts:
                self.exec_stmt(s, child)
            return None

        if isinstance(node, VarDecl):
            val = self.eval(node.value, env)
            env.set(node.name, val)
            return val

        if isinstance(node, ExprStmt):
            return self.eval(node.expr, env)

        raise HowError(f"Unknown AST node: {type(node).__name__}")

    def eval_binop(self, node: BinOp, env: Environment) -> Any:
        op = node.op
        # Short-circuit logical
        if op in ("and", "&&"):
            l = self.eval(node.left, env)
            return l if not how_truthy(l) else self.eval(node.right, env)
        if op in ("or", "||"):
            l = self.eval(node.left, env)
            return l if how_truthy(l) else self.eval(node.right, env)

        l = self.eval(node.left, env)
        r = self.eval(node.right, env)

        if op == "+":
            # List concatenation
            if isinstance(l, HowList) and isinstance(r, HowList):
                return l.concat(r)
            # String concatenation (coerce either side)
            if isinstance(l, str) or isinstance(r, str):
                return how_repr(l) + how_repr(r)
            return _num_op(l, r, op, lambda a, b: a + b)
        if op == "-":   return _num_op(l, r, op, lambda a, b: a - b)
        if op == "*":   return _num_op(l, r, op, lambda a, b: a * b)
        if op == "/":
            if r == 0:  raise HowError("Division by zero")
            return _num_op(l, r, op, lambda a, b: a / b)
        if op == "%":   return _num_op(l, r, op, lambda a, b: a % b)
        if op == "==":  return how_eq(l, r)
        if op == "!=":  return not how_eq(l, r)
        if op == "<":   return _cmp(l, r, lambda a, b: a < b)
        if op == ">":   return _cmp(l, r, lambda a, b: a > b)
        if op == "<=":  return _cmp(l, r, lambda a, b: a <= b)
        if op == ">=":  return _cmp(l, r, lambda a, b: a >= b)
        raise HowError(f"Unknown binary operator: {op!r}")

    def eval_unaryop(self, node: UnaryOp, env: Environment) -> Any:
        val = self.eval(node.operand, env)
        if node.op == "-":
            if not isinstance(val, (int, float)):
                raise HowError(f"Unary '-' requires a number")
            return -float(val)
        if node.op in ("not", "!"):
            return not how_truthy(val)
        raise HowError(f"Unknown unary op: {node.op!r}")

    def eval_assign(self, node: Assign, env: Environment) -> Any:
        val = self.eval(node.value, env)
        if isinstance(node.target, Identifier):
            name = node.target.name
            if node.op == "=":
                env.assign(name, val)
            else:
                old = env.get(name)
                env.assign(name, _apply_augop(old, val, node.op))
            return val
        if isinstance(node.target, DotAccess):
            obj  = self.eval(node.target.obj, env)
            attr = node.target.attr
            if isinstance(obj, HowInstance):
                if node.op == "=":
                    obj.set(attr, val)
                else:
                    old = obj.get(attr)
                    obj.set(attr, _apply_augop(old, val, node.op))
                return val
            raise HowError(f"Cannot assign attribute on {type(obj).__name__}")
        raise HowError(f"Invalid assignment target: {type(node.target).__name__}")

    def eval_map_lit(self, node: MapLit, env: Environment) -> Any:
        if not node.items:
            return HowMap({})
        if node.items[0][0] is None:
            # List literal
            return HowList([self.eval(v, env) for (_, v) in node.items])
        else:
            d = {}
            for k_node, v_node in node.items:
                k = self.eval(k_node, env)
                v = self.eval(v_node, env)
                d[k] = v
            return HowMap(d)

    def eval_call(self, node: Call, env: Environment) -> Any:
        callee = self.eval(node.callee, env)
        args   = [self.eval(a, env) for a in node.args]

        if callable(callee) and not isinstance(callee, (HowFunction, HowClass)):
            return callee(*args)
        if isinstance(callee, HowClass):
            return self.instantiate_class(callee, args)
        if isinstance(callee, HowFunction):
            return self.call_function(callee, args)
        if isinstance(callee, HowMap):
            if len(args) != 1:
                raise HowError("Map call requires exactly 1 argument")
            return callee.get(args[0])
        if isinstance(callee, HowList):
            if len(args) != 1:
                raise HowError("List index call requires exactly 1 argument")
            return callee.get(args[0])
        raise HowError(f"Not callable: {type(callee).__name__} ({how_repr(callee)!r})")

    def call_function(self, fn: HowFunction, args: List[Any]) -> Any:
        if fn.is_loop:
            return self.run_loop(fn)
        if len(args) != len(fn.params):
            raise HowError(f"Expected {len(fn.params)} args but got {len(args)}")
        local = Environment(fn.closure)
        for name, val in zip(fn.params, args):
            local.set(name, val)
        try:
            self.run_branches(fn.branches, local)
            return None
        except HowReturn as r:
            return r.value

    def run_branches(self, branches: List[Branch], env: Environment):
        """Execute branches in order. :: raises HowReturn. break raises HowBreak."""
        for branch in branches:
            # Local var declaration
            if isinstance(branch.body, VarDecl):
                val = self.eval(branch.body.value, env)
                env.set(branch.body.name, val)
                continue

            # Evaluate condition
            if branch.condition is not None:
                if not how_truthy(self.eval(branch.condition, env)):
                    continue

            if branch.is_return:
                val = self.eval(branch.body, env)
                raise HowReturn(val)
            else:
                self.exec_branch_body(branch.body, env)

    def exec_branch_body(self, body, env: Environment):
        if isinstance(body, Block):
            child = Environment(env)
            for s in body.stmts:
                if isinstance(s, Branch):
                    # Nested branch inside a block (cond : body syntax)
                    if s.condition is not None:
                        if not how_truthy(self.eval(s.condition, child)):
                            continue
                    if s.is_return:
                        val = self.eval(s.body, child)
                        raise HowReturn(val)
                    else:
                        self.exec_branch_body(s.body, child)
                else:
                    self.exec_stmt(s, child)
        elif isinstance(body, (VarDecl, ExprStmt)):
            self.exec_stmt(body, env)
        elif isinstance(body, BreakLoop):
            raise HowBreak()
        else:
            self.eval(body, env)

    def run_loop(self, fn: HowFunction) -> Any:
        """Run unbounded (:) or (:)= loop."""
        local = Environment(fn.closure)
        while True:
            for branch in fn.branches:
                if isinstance(branch.body, VarDecl):
                    val = self.eval(branch.body.value, local)
                    local.set(branch.body.name, val)
                    continue

                if branch.condition is not None:
                    if not how_truthy(self.eval(branch.condition, local)):
                        continue

                if branch.is_return:
                    body_val = self.eval(branch.body, local)
                    return body_val
                else:
                    try:
                        self.exec_branch_body(branch.body, local)
                    except HowBreak:
                        return None

    def run_for_loop(self, node: ForLoop, env: Environment) -> Any:
        """
        (start:stop) = varname { branches }
        Iterates varname from int(start) up to (not including) int(stop).

        Branches with :: are the POST-LOOP return value (run once, after all
        iterations, or immediately if start >= stop).  Branches with : are
        the per-iteration side-effect branches.  A :: branch that fires with
        a condition acts as an early-exit / break-and-return.
        """
        start_val = int(self.eval(node.start, env)) if node.start is not None else 0
        stop_val  = int(self.eval(node.stop,  env)) if node.stop  is not None else 0

        # Split branches into loop-body (side-effect) and final-return
        body_branches   = [b for b in node.branches if not b.is_return]
        return_branches = [b for b in node.branches if b.is_return]

        local = Environment(env)
        local.set(node.iter_var, float(start_val))

        for i in range(start_val, stop_val):
            local.assign(node.iter_var, float(i))
            try:
                self.run_branches(body_branches, local)
            except HowReturn as r:
                return r.value   # conditional :: inside loop = early exit
            except HowBreak:
                break

        # After loop completes (or breaks), evaluate post-loop return branches
        for branch in return_branches:
            if branch.condition is not None:
                if not how_truthy(self.eval(branch.condition, local)):
                    continue
            return self.eval(branch.body, local)

        return None

    def instantiate_class(self, cls: HowClass, args: List[Any]) -> Any:
        if len(args) != len(cls.params):
            raise HowError(f"Class expects {len(cls.params)} args but got {len(args)}")

        init_env = Environment(cls.closure)
        for name, val in zip(cls.params, args):
            init_env.set(name, val)

        fields = {}
        for branch in cls.branches:
            # 1. Local var declaration  (var x = ...)
            if isinstance(branch.body, VarDecl):
                val = self.eval(branch.body.value, init_env)
                init_env.set(branch.body.name, val)
                continue

            # 2. Named field  (fieldName: value  or  "key": value)
            if branch.condition is not None and not branch.is_return:
                if isinstance(branch.condition, Identifier):
                    key = branch.condition.name
                else:
                    key = self.eval(branch.condition, init_env)
                val = self.eval(branch.body, init_env)
                fields[key] = val
                continue

            # 3. Unconditional side-effect  (e.g. h.next = t)
            #    Executed immediately during class initialization.
            if branch.condition is None and not branch.is_return:
                self.exec_branch_body(branch.body, init_env)
                continue

        inst = HowInstance(fields)

        # Build a proxy env that reads/writes through the instance's fields dict.
        # This lets methods reference  size, store, head, tail, etc. as plain
        # variable names while actually mutating the shared instance fields.
        inst_env = InstanceEnv(inst, init_env)

        # Re-wrap every method so its closure is the instance-proxy env
        for k, v in list(fields.items()):
            if isinstance(v, HowFunction):
                fields[k] = HowFunction(v.params, v.branches, inst_env, v.is_loop)
        return inst


# ── Helpers ───────────────────────────────────────────────────────────────────

def how_truthy(v) -> bool:
    if v is None:   return False
    if isinstance(v, bool): return v
    if isinstance(v, (int, float)): return v != 0
    if isinstance(v, str):  return len(v) > 0
    return True

def how_eq(a, b) -> bool:
    # Both None
    if a is None and b is None: return True
    if a is None or b is None:  return False
    # Bool vs bool
    if isinstance(a, bool) and isinstance(b, bool): return a == b
    # Numeric
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return float(a) == float(b)
    # String
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    # List/Map/Instance use __eq__
    return a == b

def _num_op(l, r, op, fn):
    if not isinstance(l, (int, float)):
        raise HowError(f"Operator '{op}' requires numbers, got {type(l).__name__}")
    if not isinstance(r, (int, float)):
        raise HowError(f"Operator '{op}' requires numbers, got {type(r).__name__}")
    return float(fn(l, r))

def _cmp(l, r, fn) -> bool:
    if isinstance(l, (int,float)) and isinstance(r, (int,float)):
        return fn(float(l), float(r))
    if isinstance(l, str) and isinstance(r, str):
        return fn(l, r)
    raise HowError(f"Cannot compare {type(l).__name__} and {type(r).__name__}")

def _apply_augop(old, val, op):
    if op == "+=":
        if isinstance(old, str) or isinstance(val, str):
            return how_repr(old) + how_repr(val)
        if isinstance(old, HowList) and isinstance(val, HowList):
            return old.concat(val)
        return float(old + val)
    if op == "-=": return float(old - val)
    if op == "*=": return float(old * val)
    if op == "/=":
        if val == 0: raise HowError("Division by zero")
        return float(old / val)
    raise HowError(f"Unknown augmented op: {op!r}")
