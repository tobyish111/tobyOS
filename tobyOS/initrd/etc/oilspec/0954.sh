case $SH in zsh) exit ;; esac

# \command readonly is equivalent to \builtin declare
# except dash implements it

x='a b'

readonly b=$x
echo $b

command readonly c=$x
echo $c

\command readonly d=$x
echo $d

'command' readonly e=$x
echo $e

# The issue here is that we have a heuristic in EvalWordSequence2:
# fs len(part_vals) == 1



# note: later versions of dash are fixed
