# dash and zsh abort the whole program.   OSH doesn't?
readonly R=foo
unset R
echo status=$?
