set -o errexit
# osh aborts early here
if { echo 1; false; echo 2; set -o errexit; echo 3; false; echo 4; }; then
  echo 5;
fi
echo 6
false  # this is the one that makes shells fail
echo 7
