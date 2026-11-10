f() {
  echo f
  empty=
  return $empty
}

f
echo status=$?
