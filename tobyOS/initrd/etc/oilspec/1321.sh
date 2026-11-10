f() {
  local L1=local1
  export L1
  printenv.py L1
}
f
printenv.py L1
