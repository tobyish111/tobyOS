set -o errexit
if { echo 1; ! set +o errexit; echo 2; }; then
  echo 3
fi
echo 6
false
echo 7
