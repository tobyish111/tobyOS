set -o errexit

# bash implements inherit_errexit, but it's not as strict as OSH.
shopt -s inherit_errexit || true
shopt -s command_sub_errexit || true
echo zero
echo $(echo one; false; echo two)  # bash/ash keep going
echo parent status=$?
