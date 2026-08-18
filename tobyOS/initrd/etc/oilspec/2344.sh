case $SH in mksh) exit ;; esac

set -u

f() {
  # The temp env messes it up
  tmp1= local x=x
  tmp2= eval 'local y=y'

  # similar to eval
  tmp3= . $REPO_ROOT/spec/testdata/define-local-var-z.sh

  # Bug does not appear with only eval
  #eval 'local v=hello'

  #declare -p v
  echo x=$x
  echo y=$y
  echo z=$z
}

f 
