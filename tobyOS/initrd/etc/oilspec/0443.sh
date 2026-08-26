shopt -s expand_aliases || true
words='a b c'
alias e=export
alias r=readonly
e ex=$words
r ro=$words
argv.py "$ex" "$ro"
