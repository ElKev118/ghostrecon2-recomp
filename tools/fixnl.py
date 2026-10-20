"""Repair C string literals broken by a shell turning \\n into a real newline:
a line whose last character is inside an open double-quoted string, followed
by a line that starts with the closing quote. Joins them with a literal \\n."""
import re
import sys

for path in sys.argv[1:]:
    b = open(path, 'rb').read()
    crlf = b'\r\n' in b
    nl = b'\r\n' if crlf else b'\n'
    lines = b.split(nl)
    out = []
    fixed = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        # count unescaped quotes outside char literals (good enough for C code here)
        stripped = re.sub(rb"\\.", b"", line)
        stripped = re.sub(rb"'[^']*'", b"", stripped)
        if stripped.count(b'"') % 2 == 1 and i + 1 < len(lines) and lines[i + 1].startswith(b'"'):
            out.append(line + b'\\n' + lines[i + 1])
            i += 2
            fixed += 1
            continue
        out.append(line)
        i += 1
    if fixed:
        open(path, 'wb').write(nl.join(out))
    print(path, 'fixed', fixed)
