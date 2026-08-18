outer() {
  echo outer
  : prefix; inner() {
    echo inner
  }
}

code=$(typeset -f outer)

if false; then
  echo ---
  echo $code
  echo ---
fi

$SH -c "$code; outer; inner"
