# inspired by Perl package bug

old_dir=$(pwd)

mkdir -p cpan/Encode/Byte

# Simulate make changing the dir
wrapped_chdir() {
  #set -- $SH -c 'echo BEFORE; pwd; echo CD; cd Byte; echo AFTER; pwd'

  set -- $SH -c 'cd Byte; pwd'
  # strace comes out the same - one getcwd() and one chdir()
  #set -- strace -e 'getcwd,chdir' "$@"

  python2 -c '
from __future__ import print_function
import os, sys, subprocess

argv = sys.argv[1:]
print("Python PWD = %r" % os.getenv("PWD"), file=sys.stderr)
print("Python argv = %r" % argv, file=sys.stderr)

os.chdir("cpan/Encode")
subprocess.check_call(argv)
' "$@"
}

#wrapped_chdir
new_dir=$(wrapped_chdir)

#echo $old_dir

# Make the test insensitive to absolute paths
echo "${new_dir##$old_dir}"
