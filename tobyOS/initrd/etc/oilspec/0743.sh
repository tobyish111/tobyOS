DIR=/tmp/osh-spec-cd
mkdir -p $DIR
cd $DIR

old_dir=$(pwd)

mkdir -p cpan/Encode/Byte

# Simulate make changing the dir
wrapped_chdir() {
  #set -- $SH -c 'echo BEFORE; pwd; echo CD; cd Byte; echo AFTER; pwd'

  # disagreement before we gert here
  set -- $SH -c '
echo "PWD = $PWD"; pwd
cd Byte; echo cd=$?
echo "PWD = $PWD"; pwd
'

  # strace comes out the same - one getcwd() and one chdir()
  #set -- strace -e 'getcwd,chdir' "$@"

  python2 -c '
from __future__ import print_function
import os, sys, subprocess

argv = sys.argv[1:]
print("Python argv = %r" % argv, file=sys.stderr)

os.chdir("cpan/Encode")
print("Python PWD = %r" % os.getenv("PWD"), file=sys.stdout)
sys.stdout.flush()

subprocess.check_call(argv)
' "$@"
}

#unset PWD
wrapped_chdir
