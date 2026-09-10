#!/usr/bin/env python3
"""Grammar tests for compile_path. Run by `make test`."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "components" / "ha_action"))

import esphome.config_validation as cv  # noqa: E402

from path import compile_path  # noqa: E402

FAILURES = []


def ok(text, expected):
    try:
        got = compile_path(text)
    except cv.Invalid as err:
        FAILURES.append(f"{text!r} should compile, got error: {err}")
        return
    if got != expected:
        FAILURES.append(f"{text!r} -> {got!r}, expected {expected!r}")


def bad(text, expect_in_message=None):
    try:
        got = compile_path(text)
    except cv.Invalid as err:
        if expect_in_message and expect_in_message not in str(err):
            FAILURES.append(f"{text!r} rejected, but message lacks {expect_in_message!r}: {err}")
        return
    FAILURES.append(f"{text!r} should be rejected, compiled to {got!r}")


# The case the bracket form exists for: Home Assistant keys responses by entity
# id, and entity ids contain dots.
ok('["weather.home"].forecast[0].temperature', ["weather.home", "forecast", 0, "temperature"])
ok("['weather.home'].forecast[0]", ["weather.home", "forecast", 0])
ok("forecast[0].temperature", ["forecast", 0, "temperature"])
ok("points[1].export", ["points", 1, "export"])

# Plain keys, and a single key with no subscript at all.
ok("a", ["a"])
ok("a.b.c", ["a", "b", "c"])
ok("_leading_underscore", ["_leading_underscore"])
ok("has-hyphen.and_underscore", ["has-hyphen", "and_underscore"])

# Indices, including from the end.
ok("a[0]", ["a", 0])
ok("a[-1]", ["a", -1])
ok("a[12][3]", ["a", 12, 3])
ok('["a"]["b"]', ["a", "b"])

# A quoted key may hold anything, including the delimiter when escaped.
ok('["with space"]', ["with space"])
ok('["with.dots.and[brackets]"]', ["with.dots.and[brackets]"])
ok("[\"say \\\"hi\\\"\"]", ['say "hi"'])
ok('["0"]', ["0"])  # a quoted digit is a key, not an index

# Things that must not compile, each with a message that says why.
bad("", "empty")
bad("   ", "empty")
bad(".leading", "starts with a key")
bad("a..b", "expected a key after")
bad("a.", "expected a key after")
bad("a[", "expected an integer index or a quoted key")
bad("a[]", "expected an integer index or a quoted key")
bad("a[0", "expected an integer index or a quoted key")
bad("a[x]", "expected an integer index or a quoted key")
bad("a[0x]", "expected an integer index or a quoted key")
bad('a["unterminated]', "unterminated quoted key")
bad('a[""]', "must not be empty")
bad('a["b"', "expected ']' to close the quoted key")
bad('a["b"x]', "expected ']' to close the quoted key")
bad("a[0]b", "expected '.' or '['")
bad("a b", "expected '.' or '['")
bad("1abc", "starts with a key")

# The error must point at the offending character, or it is not worth having.
try:
    compile_path("forecast[0x].temperature")
    FAILURES.append("caret test is vacuous: that path did not raise at all")
except cv.Invalid as err:
    lines = str(err).splitlines()
    caret_line = next((line for line in lines if line.strip() == "^"), None)
    if caret_line is None:
        FAILURES.append("error has no caret line")
    elif caret_line.index("^") - 4 != len("forecast["):
        FAILURES.append(f"caret points at column {caret_line.index('^') - 4}, expected {len('forecast[')}")

if FAILURES:
    for failure in FAILURES:
        print(f"  FAIL: {failure}")
    print(f"path.py: {len(FAILURES)} test(s) failed")
    sys.exit(1)
print("path.py: all tests passed")
