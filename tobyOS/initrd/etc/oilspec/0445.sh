words='a b c'
arg_ex=ex=$words
arg_ro=ro=$words

# no quotes, this is split of course
export $arg_ex
readonly $arg_ro

argv.py "$ex" "$ro"

arg_ex2=ex2=$words
arg_ro2=ro2=$words

# quotes, no splitting
export "$arg_ex2"
readonly "$arg_ro2"

argv.py "$ex2" "$ro2"
