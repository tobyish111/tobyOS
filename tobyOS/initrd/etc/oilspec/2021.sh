#flags='--norc --noprofile'
flags='--rcfile /dev/null'

$SH $flags -i -c 'echo "_${PS1}_"'
