echo -${undef:1:2}-
set -o nounset
echo -${undef:1:2}-
echo -done-
# mksh doesn't respect nounset!
