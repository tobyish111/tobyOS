f() { 
  readonly y
  local x=1 y=$(( x ))
  echo y=$y
}
f
echo y=$y
