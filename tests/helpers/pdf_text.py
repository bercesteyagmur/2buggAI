#!/usr/bin/env python3
"""Text of a PDF written by 2buggAI's PdfReportExporter, one line per text row.

Uses `pdftotext -layout` when it is installed. Otherwise the uncompressed content streams
of the exporter are read directly: every "BT ... ET" block is one row, its "(...) Tj"
strings are joined and decoded from Windows-1252. Standard library only.

Usage: pdf_text.py <file.pdf>        (prints the text to stdout)
"""
import re
import shutil
import subprocess
import sys


def from_streams(data: bytes) -> str:
    rows = []
    for block in re.finditer(rb"BT\n(.*?)ET\n", data, re.S):
        row = bytearray()
        body = block.group(1)
        i = 0
        while i < len(body):
            if body[i:i + 1] != b"(":
                i += 1
                continue
            i += 1
            while i < len(body) and body[i:i + 1] != b")":
                c = body[i:i + 1]
                if c == b"\\":
                    nxt = body[i + 1:i + 2]
                    if nxt.isdigit():
                        row.append(int(body[i + 1:i + 4], 8))
                        i += 4
                        continue
                    row += nxt
                    i += 2
                    continue
                row += c
                i += 1
            i += 1
        rows.append(row.decode("cp1252", errors="replace"))
    return "\n".join(rows) + "\n"


def main() -> int:
    path = sys.argv[1]
    if shutil.which("pdftotext"):
        out = subprocess.run(["pdftotext", "-layout", path, "-"], capture_output=True, text=True)
        if out.returncode == 0:
            sys.stdout.write(out.stdout)
            return 0
    with open(path, "rb") as f:
        sys.stdout.write(from_streams(f.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
