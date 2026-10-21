#!/usr/bin/env python3
"""Every CoreBridge.<name> in the QML matches corebridge.h.

A Q_INVOKABLE referenced without its parentheses is not an error in
QML: it evaluates to the function object, and QML happily puts
"function() { [native code] }" on screen. That is how the About
section came to read "configured function() { [native code] }".

The reverse is as quiet: a Q_PROPERTY called like a method is
undefined, and a name that is neither is undefined too. None of the
three is a warning at load, so nothing said so until somebody looked
at a screenshot.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QUICK = os.path.join(HERE, "..", "..", "frontend", "quick")


def strip_comments(text):
    """QML comments, so a name inside one is not a reference."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main():
    header = open(os.path.join(QUICK, "corebridge.h")).read()
    invokables = set(
        re.findall(r"Q_INVOKABLE\s+[\w:<>,\s\*&]+?\b(\w+)\s*\(", header))
    invokables |= set(re.findall(r"Q_PROPERTY\([^)]*?READ\s+(\w+)", header))
    properties = set(re.findall(r"Q_PROPERTY\(\s*\S+\s+(\w+)", header))
    invokables -= properties

    problems = []
    for name in sorted(os.listdir(QUICK)):
        if not name.endswith(".qml"):
            continue
        path = os.path.join(QUICK, name)
        body = strip_comments(open(path).read())
        for lineno, line in enumerate(body.split("\n"), 1):
            for m in re.finditer(r"CoreBridge\.(\w+)\s*(\(?)", line):
                who, paren = m.group(1), m.group(2)
                if who in invokables and paren != "(":
                    problems.append("%s:%d: CoreBridge.%s is a method; it "
                                    "needs ()" % (name, lineno, who))
                elif who in properties and paren == "(":
                    problems.append("%s:%d: CoreBridge.%s is a property; it "
                                    "is not called" % (name, lineno, who))
                elif who not in invokables and who not in properties:
                    problems.append("%s:%d: CoreBridge.%s is not in "
                                    "corebridge.h" % (name, lineno, who))

    for p in problems:
        print("FAIL " + p)
    print("%d invokables, %d properties, %d problems"
          % (len(invokables), len(properties), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
