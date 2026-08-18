# WORKAROUND for Python version of bin/osh -- we can't run bin/oils_for_unix.py
# because it a shebang #!/usr/bin/env python2
# This test is still useful for the C++ oils-for-unix.

case $SH in
  */bin/osh)
    echo yes
    echo yes
    exit
    ;;
esac

# Get absolute path before changing PATH
sh=$(which $SH)

old_path=$PATH
unset PATH

$sh -c 'echo $PATH' > path.txt

PATH=$old_path

# looks like PATH=/usr/bin:/bin for mksh, but more complicated for others
# cat path.txt

# should contain /usr/bin
if egrep -q '(^|:)/usr/bin($|:)' path.txt; then
  echo yes
fi

# should contain /bin
if egrep -q '(^|:)/bin($|:)' path.txt ; then
  echo yes
fi
