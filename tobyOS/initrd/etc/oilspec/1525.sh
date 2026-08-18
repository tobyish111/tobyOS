set -o errexit
shopt -s inherit_errexit || true
#shopt -s strict_errexit || true
shopt -s command_sub_errexit || true

myproc() {
  # this is disallowed because we want a runtime error 100% of the time
  local x=$(true)

  # Realistic example.  Should fail here but shells don't!
  local d=$(date %x)
  echo hi
}
myproc
