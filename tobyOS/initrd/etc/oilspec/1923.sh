# bash and dash disagree on exit code.
f() {
  local FOO-BAR=foo
}
f
