case $SH in mksh|zsh|dash|ash) echo not implemented; exit ;; esac
# OK so printf is like assigning to a var.
# printf -v foo %q "$bar" is like
# foo=${bar@Q}
dollar='dollar'
f() {
  local mylocal=foo
  printf -v dollar %q '$'  # assign foo to a quoted dollar
  printf -v mylocal %q 'mylocal'
  echo dollar=$dollar
  echo mylocal=$mylocal
}
echo dollar=$dollar
echo --
f
echo --
echo dollar=$dollar
echo mylocal=$mylocal
