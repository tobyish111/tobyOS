vec2_load() {
  local this=$1
  x=$(( ${this}_x ))
  : $(( y = ${this}_y ))
}
a_x=12 a_y=34
vec2_load a
echo x=$x y=$y
