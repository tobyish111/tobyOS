case $SH in ash) exit ;; esac

err() {
  echo "err [$@] status=$? [${PIPESTATUS[@]}]"
}
trap 'err' ERR 

echo A

false

# succeeds
echo B | grep B

# fails
echo C | grep zzz

echo D | grep zzz | cat

set -o pipefail
echo E | grep zzz | cat

trap - ERR  # disable trap

echo F | grep zz
echo ok


# we don't set PIPESTATUS unless we get a pipeline
