#!/usr/bin/env python3
"""Stand-in for `tsc --noEmit --pretty false`, used by run_fullstack_test.sh ONLY when
Node.js is not installed.

It is not a type checker. It knows one rule that is enough for the fixture: a property
read on a variable called `user` must be a field of `interface User` in src/types.ts.
Violations are printed in the exact format of tsc, so the real parser, checklist and
cross-component context are exercised. Exit code 2 on errors, like tsc.
"""
import pathlib
import re
import sys

src = pathlib.Path("src")
interface = re.search(r"interface User\s*\{(.*?)\}", (src / "types.ts").read_text(), re.S)
fields = set(re.findall(r"^\s*(\w+)\??:", interface.group(1), re.M)) if interface else set()

errors = 0
for path in sorted(src.glob("*.ts")):
    if ".test." in path.name:
        continue
    for number, line in enumerate(path.read_text().splitlines(), 1):
        for match in re.finditer(r"\buser\.(\w+)", line):
            if match.group(1) not in fields:
                print(f"{path.as_posix()}({number},{match.start(1) + 1}): error TS2339: "
                      f"Property '{match.group(1)}' does not exist on type 'User'.")
                errors += 1
sys.exit(2 if errors else 0)
