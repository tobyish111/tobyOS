# Under extdebug each function and each sourced file is a frame of
# BASH_ARGC and BASH_ARGV, over the arguments there were when extdebug was
# set -- tsh kept the script's frame only (finding 537).
exec 2>&1
set -- a b
shopt -s extdebug
cs() { echo "${BASH_ARGC[*]} / ${BASH_ARGV[*]}"; }
f3() { cs; }
f2() { f3 3 z; }
f1() { f2 2 y; }
f1 1 x
set -- c
cs q
echo 'echo "in: ${BASH_ARGC[*]} / ${BASH_ARGV[*]}"' > a.sub
g() { . ./a.sub p q; }
g m
. ./a.sub
shopt -u extdebug
h() { cs; }
h r
