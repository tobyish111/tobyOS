myfunc() { echo x; }

shopt -s expand_aliases
alias ll='ls -l'

backtick=\`
command -V ll | sed "s/$backtick/'/g"
echo status=$?

command -V echo
echo status=$?

# Paper over insignificant difference
command -V myfunc | sed 's/shell function/function/'
echo status=$?

command -V nonexistent  # doesn't print anything
echo status=$?

command -V for
echo status=$?
