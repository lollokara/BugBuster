import Foundation

/// The MicroPython built-ins the on-device interpreter provides (functions, types,
/// exceptions, keywords), for the editor's completion. A small hand-kept subset:
/// no `open`, `input` or `compile`, since the board has no filesystem or stdin.
enum ScriptBuiltins {
    enum Kind: Equatable {
        case function, type, exception, keyword
    }

    struct Entry: Equatable {
        let name: String
        let kind: Kind
        let signature: String
        let doc: String
        /// Parameters for functions and types; empty for exceptions and keywords.
        let params: [FirmwareParam]

        /// The call as a `FirmwareFunction`, so insertion reuses the catalogue's placeholder logic.
        var function: FirmwareFunction? {
            kind == .function || kind == .type
                ? FirmwareFunction(name: name, signature: signature, params: params, returns: nil, doc: doc)
                : nil
        }
    }

    static let all: [Entry] = functions + types + exceptions + keywords

    static func entry(named name: String) -> Entry? { byName[name] }

    private static let byName: [String: Entry] = Dictionary(all.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })

    /// Python-like parameter list: `x, y=1, *rest, flag=False, **kw`. A bare `*` starts the
    /// keyword-only parameters; `/` is ignored.
    static func parseParams(_ spec: String) -> [FirmwareParam] {
        var out: [FirmwareParam] = []
        var keywordOnly = false
        for raw in spec.split(separator: ",") {
            let token = raw.trimmingCharacters(in: .whitespaces)
            switch token {
            case "": continue
            case "/": continue
            case "*": keywordOnly = true; continue
            default: break
            }
            if token.hasPrefix("**") {
                out.append(FirmwareParam(name: String(token.dropFirst(2)), kind: .varKeyword, annotation: nil, defaultValue: nil))
            } else if token.hasPrefix("*") {
                keywordOnly = true
                out.append(FirmwareParam(name: String(token.dropFirst()), kind: .varPositional, annotation: nil, defaultValue: nil))
            } else {
                let parts = token.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
                out.append(FirmwareParam(name: parts[0], kind: keywordOnly ? .keywordOnly : .positional,
                                         annotation: nil, defaultValue: parts.count > 1 ? parts[1] : nil))
            }
        }
        return out
    }

    private static func call(_ name: String, _ spec: String, _ doc: String, kind: Kind = .function) -> Entry {
        Entry(name: name, kind: kind, signature: "\(name)(\(spec))", doc: doc, params: parseParams(spec))
    }

    private static func exception(_ name: String, _ doc: String) -> Entry {
        Entry(name: name, kind: .exception, signature: name, doc: doc, params: [])
    }

    private static func keyword(_ name: String, _ doc: String) -> Entry {
        Entry(name: name, kind: .keyword, signature: name, doc: doc, params: [])
    }

    private static let functions: [Entry] = [
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
        call("zip", "*iterables", "Iterate several iterables in parallel as tuples.")
    ]

    private static let types: [Entry] = [
        call("bool", "x", "Truth value of x.", kind: .type),
        call("bytearray", "source=b''", "Mutable byte sequence.", kind: .type),
        call("bytes", "source=b''", "Immutable byte sequence, e.g. bytes([0x9f, 0x00]).", kind: .type),
        call("dict", "**kwargs", "Create a dictionary.", kind: .type),
        call("enumerate", "iterable, start=0", "Pairs of (index, item).", kind: .type),
        call("float", "x", "Convert to a floating-point number.", kind: .type),
        call("frozenset", "iterable=()", "Immutable set.", kind: .type),
        call("int", "x, base=10", "Convert to an integer.", kind: .type),
        call("list", "iterable=()", "Create a list.", kind: .type),
        call("memoryview", "obj", "View of a buffer without copying.", kind: .type),
        call("object", "", "Base class of every class.", kind: .type),
        call("range", "stop", "Sequence of integers: range(stop), range(start, stop[, step]).", kind: .type),
        call("set", "iterable=()", "Create a set.", kind: .type),
        call("str", "obj", "Convert to a string.", kind: .type),
        call("tuple", "iterable=()", "Create a tuple.", kind: .type)
    ]

    private static let exceptions: [Entry] = [
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
        exception("ZeroDivisionError", "Division by zero.")
    ]

    private static let keywords: [Entry] = [
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
        keyword("yield", "Produce a value from a generator.")
    ]
}
