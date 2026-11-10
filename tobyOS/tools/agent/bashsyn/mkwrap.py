"""mkwrap.py SRC DST -- wrap every SRC/*.sh as the body of a function, so that
bash's `declare -f f` prints bash's own reconstruction of it (print_cmd.c).
The printer's oracle: oracle-wrap.sh in WSL over DST, compared with
prharness's printing of the same files (cmp.sh)."""
import os, sys
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
n = 0
for name in sorted(os.listdir(src)):
    if not name.endswith('.sh'):
        continue
    body = open(os.path.join(src, name), 'rb').read()
    if not body.endswith(b'\n'):
        body += b'\n'
    out = b'f() {\n' + body + b'}\ndeclare -f f\n'
    open(os.path.join(dst, name), 'wb').write(out)
    n += 1
print(n)
