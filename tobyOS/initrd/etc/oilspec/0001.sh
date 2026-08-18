shopt -s expand_aliases || true
alias -- foo=echo
echo status=$?
foo x
unalias -- foo
foo x
# dash doesn't accept --
