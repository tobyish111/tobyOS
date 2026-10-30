# A list's [key]=value finds the `]` that closes its key past a quoted or
# escaped bracket and past a command substitution: `["a]b"]=1`, `[c\]d]=2`,
# `["bar[bie"]=doll` (finding 366). tsh stored such elements as bare words.
exec 2>&1
declare -A D=(["bar[bie"]="doll"); declare -p D
declare -A H; H=( ["a]b"]=1 [c\]d]=2 ['e]f']=3 ); declare -p H
declare -A E; E+=([\`]= [\]]=); declare -p E
declare -A J; J=( [$(echo ])]=v ); declare -p J
