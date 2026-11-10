touch _tmp/b.B
fun() {
  echo $@
}
fun '_tmp/*.B'
