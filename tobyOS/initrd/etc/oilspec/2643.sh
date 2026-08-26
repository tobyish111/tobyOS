flags=''
case $SH in
  dash) exit ;;
  bash*)
    flags='--noprofile --norc --rcfile /devnull'
    ;;
  osh)
    flags='--rcfile /devnull'
    ;;
esac

sh_path=$(which $SH)

case $sh_path in
  */bin/osh)
    # Hack for running with Python2
    export PYTHONPATH="$REPO_ROOT:$REPO_ROOT/vendor"
    sh_prefix="$(which python2) $REPO_ROOT/bin/oils_for_unix.py osh"
    ;;
  *)
    sh_prefix=$sh_path
    ;;
esac

#echo PATH=$PATH


# mksh has typeset, not declare
# bash exports PWD, but not PATH PS4

/usr/bin/env -i PYTHONPATH=$PYTHONPATH $sh_prefix $flags -c 'typeset -p PATH PWD PS4' >&2
echo path pwd ps4 $?

/usr/bin/env -i PYTHONPATH=$PYTHONPATH $sh_prefix $flags -c 'typeset -p SHELLOPTS' >&2
echo shellopts $?

# bash doesn't set HOME, mksh and zsh do
/usr/bin/env -i PYTHONPATH=$PYTHONPATH $sh_prefix $flags -c 'typeset -p HOME PS1' >&2
echo home ps1 $?

# IFS is set, but not exported
/usr/bin/env -i PYTHONPATH=$PYTHONPATH $sh_prefix $flags -c 'typeset -p IFS' >&2
echo ifs $?
