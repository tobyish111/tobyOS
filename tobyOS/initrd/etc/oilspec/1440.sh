shopt -u strict_arith || true
decimal=15
octal=017   # = 15 (decimal)
[[ $decimal -eq $octal ]] && echo true
[[ $decimal -eq ZZZ$octal ]] || echo false
# mksh doesn't implement this syntax for literals.
