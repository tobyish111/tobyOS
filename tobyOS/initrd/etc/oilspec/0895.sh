# Wow this is poorly specified!  A fundamental design problem with the global
# variable OPTIND.
set -- -a
while getopts "ab:" opt; do
  echo '.'
done
echo OPTIND=$OPTIND

set -- -c -d -e foo
while getopts "cde:" opt; do
  echo '-'
done
echo OPTIND=$OPTIND

set -- -f
while getopts "f:" opt; do
  echo '_'
done
echo OPTIND=$OPTIND
