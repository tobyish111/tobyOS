# The other shells don't let you recover from this programming error!
set -o errexit
for i in $(seq 2); do
  echo "> $i"
  ( if true; then continue; fi; echo "Should not print" )
  echo 'should fail after subshell'
  echo ". $i"
done


> 1
should fail after subshell
. 1
> 2
should fail after subshell
. 2
