readonly x=1
x=2; echo same-line
echo next-line
set -o posix
f() { x=3; echo in-f; }; f
echo after-f
