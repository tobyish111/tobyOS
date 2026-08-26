case $SH in dash|bash|mksh|ash) exit ;; esac

shopt -s parse_brace strict_errexit || true

p() {
  echo before
  local x
  # This line fails, which is a bit weird, but errexit
  x=$(false)
  echo x=$x
}

set -o errexit
shopt --unset errexit {
  # It runs normally here, because errexit was disabled (just not by a
  # conditional)
  p
}
