# A function definition is a command, so it can be nested
fun() {
  nested_func() { echo nested; }
  nested_func
}
fun
