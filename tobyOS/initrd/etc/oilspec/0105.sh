check() {
  $SH -c "shopt --set strict_arith; echo $1"
  echo status=$?
}

check '$(( 0x1X ))'
check '$(( 09 ))'
check '$(( 2#A ))'
check '$(( 02#0110 ))'
