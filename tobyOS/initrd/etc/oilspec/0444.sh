words='a b c'
e=export
r=readonly
$e ex=$words
$r ro=$words
argv.py "$ex" "$ro"

# zsh and OSH are smart
