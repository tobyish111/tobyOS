# bash and dash trace this badly.  They print literal newlines, which I don't
# want.
set -x
echo $'[\n]'
# bash has ugly output that spans lines
