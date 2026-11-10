# With expand_aliases off an alias is no command: `command -v m` prints
# nothing, status 1, as type finds nothing -- tsh printed the alias
# (finding 514).
exec 2>&1
alias m='more'
command -v m; echo "rc=$?"
command -V m; echo "rc=$?"
shopt -s expand_aliases
command -v m; echo "rc=$?"
command -V m; echo "rc=$?"
