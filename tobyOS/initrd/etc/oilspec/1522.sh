set -o errexit
set -o pipefail

f() {
  local dir=$1
	if ls $dir | grep ''; then
    echo foo
		echo ${PIPESTATUS[@]}
	fi
}
rmdir $TMP/_tmp || true
rm -f $TMP/*
f $TMP
f /nonexistent # should fail
echo done
