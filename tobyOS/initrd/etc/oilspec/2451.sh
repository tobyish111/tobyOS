echo -${v/x/y}-
echo status=$?
set -o nounset  # make sure this fails
echo -${v/x/y}-
