case $SH in zsh) exit ;; esac

export PS1='[PS1]'

echo 'if true
then
  echo hi
fi' | $SH -i

if test -z "$OILS_VERSION"; then
  echo '^D'  # fudge
fi



# hm somehow bash prints it more nicely; code is echo'd to stderr
