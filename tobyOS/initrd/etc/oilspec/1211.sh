trap 'echo line=$LINENO' ERR

f() {
  false 
  true
}

f
