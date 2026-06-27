#!/usr/bin/env python3
# realpython proof script: exercises the REAL CPython interpreter end-to-end --
# generator expressions, builtins, integer arithmetic, string formatting, and a
# stdlib import loaded from the zipimport archive (json is pure-python and lives
# in /lib/python310.zip, so a successful import proves zipimport works).
import sys
import json

squares = [i * i for i in range(1, 8)]      # 1,4,9,16,25,36,49
total = sum(squares)                        # = 140
blob = json.dumps({"sum": total, "n": len(squares)}, sort_keys=True)

ver = sys.version.split()[0]
print("PYTHON-OK ver=%s sum=%d json=%s" % (ver, total, blob))

# Exit code proves the script actually computed something (140 - 98 = 42),
# not just that the interpreter started.
sys.exit(total - 98)
