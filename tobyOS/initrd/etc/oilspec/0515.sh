f2() { builtin return 3;  echo unexpected; return 0; }
f3() { \builtin return 3; echo unexpected; return 0; }
f4() { command return 3;  echo unexpected; return 0; }
f2; echo "status=$?"
f3; echo "status=$?"
f4; echo "status=$?"
# Note: zsh does not allow calling builtin through command
# Note: ash does not have "builtin"
