# Implementation quirk:
# - The proc check happens only if errexit WAS on and is disabled
# - But 'shopt --unset allow_csub_psub' happens if it was never on

shopt -s strict_errexit || true

p() {
  echo before
  local x
  # This line fails, which is a bit weird, but errexit
  x=$(false)
  echo x=$x
}

if p; then
  echo ok
fi
