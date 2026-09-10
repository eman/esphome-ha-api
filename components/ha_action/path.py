"""The `path:` grammar, compiled at config time.

Home Assistant keys its action responses by entity id, and entity ids contain
dots. `weather.get_forecasts` returns

    {"weather.home": {"forecast": [{"datetime": ..., "temperature": 14.2}]}}

so a bare dotted path would be ambiguous on the very first segment of the most
common response shape there is. The grammar:

    path      := first ( '.' ident | '[' subscript ']' )*
    first     := ident | '[' subscript ']'
    subscript := integer | quoted-string
    ident     := [A-Za-z_][A-Za-z0-9_-]*

Compiling here rather than on the device means a malformed path is a config
error with a caret under the offending character, and the C++ side only ever
walks a token list - so there is exactly one implementation of the grammar.
"""

import re

import esphome.config_validation as cv

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_\-]*")
_INT = re.compile(r"-?\d+")

_HINT = (
    'Paths look like `forecast[0].temperature`, or `["weather.home"].forecast[0].temperature` '
    "when a key contains dots - which Home Assistant response keys usually do, "
    "because they are entity ids."
)


def compile_path(value):
    """A path string -> a list of tokens: str for a key, int for an index."""
    s = cv.string_strict(value)
    if not s.strip():
        raise cv.Invalid(f"path must not be empty. {_HINT}")

    n = len(s)
    tokens: list[str | int] = []

    def fail(pos: int, msg: str):
        caret = " " * max(0, min(pos, n)) + "^"
        raise cv.Invalid(f"{msg}\n    {s}\n    {caret}\n  {_HINT}")

    def read_bracket(start: int):
        """s[start] == '['. Returns (token, index just past the ']')."""
        j = start + 1
        if j < n and s[j] in "\"'":
            quote = s[j]
            j += 1
            buf = []
            while j < n and s[j] != quote:
                # A backslash escapes the next character, so a key may contain
                # the quote it is delimited by.
                if s[j] == "\\" and j + 1 < n:
                    j += 1
                buf.append(s[j])
                j += 1
            if j >= n:
                fail(start + 1, "unterminated quoted key - no closing quote")
            key = "".join(buf)
            if not key:
                fail(start + 1, "a quoted key must not be empty")
            j += 1
            if j >= n or s[j] != "]":
                fail(j, "expected ']' to close the quoted key")
            return key, j + 1

        m = _INT.match(s, j)
        if m is None or m.end() >= n or s[m.end()] != "]":
            fail(j, 'expected an integer index or a quoted key, like [0], [-1] or ["sensor.x"]')
        return int(m.group(0)), m.end() + 1

    if s[0] == "[":
        token, i = read_bracket(0)
        tokens.append(token)
    else:
        m = _IDENT.match(s)
        if m is None:
            fail(0, "a path starts with a key or a '['")
        tokens.append(m.group(0))
        i = m.end()

    while i < n:
        if s[i] == "[":
            token, i = read_bracket(i)
            tokens.append(token)
        elif s[i] == ".":
            m = _IDENT.match(s, i + 1)
            if m is None:
                fail(i + 1, "expected a key after '.'")
            tokens.append(m.group(0))
            i = m.end()
        else:
            fail(i, f"expected '.' or '[' here, not {s[i]!r}")

    return tokens


def add_path(var, tokens) -> None:
    """Emit a compiled path onto a C++ object exposing add_path_key/index."""
    import esphome.codegen as cg

    for token in tokens:
        if isinstance(token, int):
            cg.add(var.add_path_index(token))
        else:
            cg.add(var.add_path_key(token))
