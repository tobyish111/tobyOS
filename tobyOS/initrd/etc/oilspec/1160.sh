set -a
f() {
  local ZZZ=zzz
  printenv.py ZZZ
}
f
