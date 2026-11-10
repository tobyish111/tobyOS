shopt -u strict_arith || true
decimal=15
hex=0x0f    # = 15 (decimal)
[[ $decimal -eq $hex ]] && echo true
[[ $decimal -eq ZZZ$hex ]] || echo false

# TODO: Add tests for this
# https://www.gnu.org/software/bash/manual/bash.html#Bash-Conditional-Expressions
# When used with [[, the ‘<’ and ‘>’ operators sort lexicographically using the
# current locale. The test command uses ASCII ordering.
