type zz 2>err.txt
echo status=$?

# for bash and OSH: print to stderr
fgrep -o 'zz: not found' err.txt || true

# zsh and mksh behave the same - status 1
# dash and ash behave the same - status 127
