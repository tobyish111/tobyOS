case $SH in dash) exit ;; esac

err() {
  echo err status $?
  false
  ( exit 2 )  # not recursively triggered
  echo err 2
}
trap 'err' ERR 

echo A
false
echo B

# Try it with errexit
set -e
false
echo C
