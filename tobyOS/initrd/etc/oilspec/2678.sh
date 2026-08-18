case $SH in
  bash*)
    # BASH_VERSION=zz

    echo $BASH_VERSION | egrep -o '4\.4\.0' > /dev/null
    echo matched=$?
    ;;
  *osh)
    # note: version string is mutable like in bash.  I guess that's useful for
    # testing?  We might want a strict mode to eliminate that?

    echo $OILS_VERSION | egrep -o '[0-9]+\.[0-9]+\.' > /dev/null
    echo matched=$?
    ;;
  *)
    echo 'no version'
    ;;
esac
