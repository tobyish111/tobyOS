_comp_cmd_test() {
  unset -v COMPREPLY
  COMPREPLY=hello
}
compgen -F _comp_cmd_test
