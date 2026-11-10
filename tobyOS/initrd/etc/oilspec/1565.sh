echo ft $(false) $(true)
echo status=$?

set -o errexit
shopt -s inherit_errexit || true

# This changes it
#shopt -s command_sub_errexit || true

echo f $(date %x)
echo status=$?

# compare with 
# x=$(date %x)         # FAILS
# local x=$(date %x)   # does NOT fail

echo ft $(false) $(true)
echo status=$?
