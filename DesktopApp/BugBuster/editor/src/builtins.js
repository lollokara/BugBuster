// MicroPython built-ins the on-device interpreter provides, for completion. Mirrors
// iOSApp/Sources/Services/ScriptBuiltins.swift: a small hand-kept subset (no open/input/compile).

/** Python-like spec `x, y=1, *rest, flag=False, **kw`; a bare `*` starts keyword-only, `/` is ignored. */
export function parseParams(spec) {
  const out = [];
  let keywordOnly = false;
  const make = (name, kind, defaultValue) => {
    const acceptsKeyword = kind === "positional" || kind === "keyword_only";
    return { name, kind, annotation: null, defaultValue, doc: "", acceptsKeyword, isRequired: defaultValue === null && acceptsKeyword };
  };
  for (const raw of spec.split(",")) {
    const token = raw.trim();
    if (token === "" || token === "/") continue;
    if (token === "*") {
      keywordOnly = true;
    } else if (token.startsWith("**")) {
      out.push(make(token.slice(2), "var_keyword", null));
    } else if (token.startsWith("*")) {
      keywordOnly = true;
      out.push(make(token.slice(1), "var_positional", null));
    } else {
      const eq = token.indexOf("=");
      const name = (eq < 0 ? token : token.slice(0, eq)).trim();
      const def = eq < 0 ? null : token.slice(eq + 1).trim();
      out.push(make(name, keywordOnly ? "keyword_only" : "positional", def));
    }
  }
  return out;
}

function entry(name, kind, signature, doc, params) {
  const fn =
    kind === "function" || kind === "type"
      ? { name, signature, params, returns: null, doc, detail: null, summary: doc, owner: null }
      : null;
  return { name, kind, signature, doc, params, function: fn };
}

const call = (name, spec, doc, kind = "function") => entry(name, kind, `${name}(${spec})`, doc, parseParams(spec));
const exception = (name, doc) => entry(name, "exception", name, doc, []);
const keyword = (name, doc) => entry(name, "keyword", name, doc, []);

const functions = [
  call("abs", "x", "Absolute value of a number."),
  call("all", "iterable", "True if every item is true (or the iterable is empty)."),
  call("any", "iterable", "True if any item is true."),
  call("bin", "x", "Integer as a binary string, e.g. '0b101'."),
  call("callable", "obj", "True if the object can be called."),
  call("chr", "i", "One-character string for a code point."),
  call("dir", "obj", "Names of an object's attributes."),
  call("divmod", "a, b", "Quotient and remainder as a tuple."),
  call("eval", "source", "Evaluate an expression string."),
  call("exec", "source", "Run a string of statements."),
  call("filter", "function, iterable", "Items for which the function is true."),
  call("getattr", "obj, name, default=None", "Attribute by name, or the default."),
  call("globals", "", "The global namespace as a dict."),
  call("hasattr", "obj, name", "True if the object has the attribute."),
  call("hash", "obj", "Hash value of an object."),
  call("hex", "x", "Integer as a hex string, e.g. '0xff'."),
  call("id", "obj", "Identity of an object."),
  call("isinstance", "obj, classinfo", "True if obj is an instance of the class (or tuple of classes)."),
  call("issubclass", "cls, classinfo", "True if cls is a subclass of the class (or tuple of classes)."),
  call("iter", "obj", "Iterator over an object."),
  call("len", "obj", "Number of items in a container or characters in a string."),
  call("locals", "", "The local namespace as a dict."),
  call("map", "function, iterable", "Apply a function to every item."),
  call("max", "iterable, *, key=None, default=None", "Largest item (or the largest of several arguments)."),
  call("min", "iterable, *, key=None, default=None", "Smallest item (or the smallest of several arguments)."),
  call("next", "iterator, default=None", "Next item of an iterator, or the default when exhausted."),
  call("oct", "x", "Integer as an octal string."),
  call("ord", "c", "Code point of a one-character string."),
  call("pow", "x, y, mod=None", "x to the power y, optionally modulo mod."),
  call("print", "*objects, sep=' ', end='\\n'", "Print values to the console."),
  call("repr", "obj", "Printable representation of an object."),
  call("reversed", "seq", "Iterator over a sequence in reverse."),
  call("round", "number, ndigits=None", "Round to ndigits decimal places."),
  call("setattr", "obj, name, value", "Set an attribute by name."),
  call("sorted", "iterable, *, key=None, reverse=False", "New sorted list."),
  call("sum", "iterable, start=0", "Sum of the items plus start."),
  call("type", "obj", "The type of an object."),
  call("zip", "*iterables", "Iterate several iterables in parallel as tuples."),
];

const types = [
  call("bool", "x", "Truth value of x.", "type"),
  call("bytearray", "source=b''", "Mutable byte sequence.", "type"),
  call("bytes", "source=b''", "Immutable byte sequence, e.g. bytes([0x9f, 0x00]).", "type"),
  call("dict", "**kwargs", "Create a dictionary.", "type"),
  call("enumerate", "iterable, start=0", "Pairs of (index, item).", "type"),
  call("float", "x", "Convert to a floating-point number.", "type"),
  call("frozenset", "iterable=()", "Immutable set.", "type"),
  call("int", "x, base=10", "Convert to an integer.", "type"),
  call("list", "iterable=()", "Create a list.", "type"),
  call("memoryview", "obj", "View of a buffer without copying.", "type"),
  call("object", "", "Base class of every class.", "type"),
  call("range", "stop", "Sequence of integers: range(stop), range(start, stop[, step]).", "type"),
  call("set", "iterable=()", "Create a set.", "type"),
  call("str", "obj", "Convert to a string.", "type"),
  call("tuple", "iterable=()", "Create a tuple.", "type"),
];

const exceptions = [
  exception("ArithmeticError", "Base class of arithmetic errors."),
  exception("AssertionError", "An assert statement failed."),
  exception("AttributeError", "An attribute does not exist."),
  exception("BaseException", "Base class of all exceptions."),
  exception("EOFError", "End of input reached."),
  exception("Exception", "Base class of ordinary exceptions."),
  exception("ImportError", "A module could not be imported."),
  exception("IndentationError", "Source indentation is wrong."),
  exception("IndexError", "A sequence index is out of range."),
  exception("KeyboardInterrupt", "The script was stopped (Stop button)."),
  exception("KeyError", "A dictionary key is missing."),
  exception("LookupError", "Base class of IndexError and KeyError."),
  exception("MemoryError", "Out of heap memory."),
  exception("NameError", "A name is not defined."),
  exception("NotImplementedError", "A feature is not implemented."),
  exception("OSError", "A system or hardware error; carries an errno."),
  exception("OverflowError", "A number is too large."),
  exception("RuntimeError", "A general run-time error."),
  exception("StopIteration", "An iterator has no more items."),
  exception("SyntaxError", "The source is not valid Python."),
  exception("SystemExit", "Raised by sys.exit()."),
  exception("TypeError", "A value has the wrong type."),
  exception("UnicodeError", "A string encoding problem."),
  exception("ValueError", "A value is out of range or malformed."),
  exception("ZeroDivisionError", "Division by zero."),
];

const keywords = [
  keyword("and", "Logical and."),
  keyword("as", "Alias in import / with / except."),
  keyword("assert", "Raise AssertionError when a condition is false."),
  keyword("async", "Define an asynchronous function."),
  keyword("await", "Wait for an awaitable."),
  keyword("break", "Leave the innermost loop."),
  keyword("class", "Define a class."),
  keyword("continue", "Next iteration of the loop."),
  keyword("def", "Define a function."),
  keyword("del", "Delete a name, item or attribute."),
  keyword("elif", "Else-if branch."),
  keyword("else", "Fallback branch."),
  keyword("except", "Handle an exception."),
  keyword("False", "Boolean false."),
  keyword("finally", "Block that always runs after try."),
  keyword("for", "Loop over an iterable."),
  keyword("from", "Import names from a module."),
  keyword("global", "Refer to a module-level name."),
  keyword("if", "Conditional branch."),
  keyword("import", "Import a module."),
  keyword("in", "Membership test / loop source."),
  keyword("is", "Identity test."),
  keyword("lambda", "Anonymous function."),
  keyword("None", "The null value."),
  keyword("nonlocal", "Refer to an enclosing function's name."),
  keyword("not", "Logical not."),
  keyword("or", "Logical or."),
  keyword("pass", "Do nothing."),
  keyword("raise", "Raise an exception."),
  keyword("return", "Return from a function."),
  keyword("True", "Boolean true."),
  keyword("try", "Start an exception-handling block."),
  keyword("while", "Loop while a condition holds."),
  keyword("with", "Run a block inside a context manager."),
  keyword("yield", "Produce a value from a generator."),
];

export const BUILTINS = [...functions, ...types, ...exceptions, ...keywords];

const byName = new Map();
for (const e of BUILTINS) if (!byName.has(e.name)) byName.set(e.name, e);

export function builtinNamed(name) {
  return byName.get(name) ?? null;
}
