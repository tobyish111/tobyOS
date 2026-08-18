# bash, mksh, and zsh all treat strings that don't look like numbers as zero.
shopt -u strict_arith || true
s=foo
echo $((s+5))
