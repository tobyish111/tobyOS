# NOTE: This changes things
# set -e

cd $REPO_ROOT
f() {
  for i in $(seq 5); do 
    if test $i = 2; then
      . spec/testdata/continue.sh
    fi
    if test $i = 4; then
      . spec/testdata/break.sh
    fi
    echo $i
  done

  # Return is different!
  . spec/testdata/return.sh
  echo done
}
f
