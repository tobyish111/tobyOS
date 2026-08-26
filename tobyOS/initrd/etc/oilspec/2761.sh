# osh shows inline error; maybe fail like dash/mksh?
x=1
PS4='+${x'
set -o xtrace
echo one
echo status=$?
# mksh and dash both fail.  bash prints errors to stderr.
