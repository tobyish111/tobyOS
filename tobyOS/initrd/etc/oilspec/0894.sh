while getopts "hc:" opt; do
  echo '-'
done
echo OPTIND=$OPTIND

set -- -h -c foo x y z
while getopts "hc:" opt; do
  echo '-'
done
echo OPTIND=$OPTIND

set --
while getopts "hc:" opt; do
  echo '-'
done
echo OPTIND=$OPTIND
