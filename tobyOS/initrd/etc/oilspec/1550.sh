set -o errexit
if { echo 1; false; echo 2; set +o errexit; echo 3; false; echo 4; }; then
  echo 5;
fi
echo 6
false  # does NOT fail, because we restored it.
echo 7
