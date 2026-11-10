# Reading an associative element whose key is written with an escaped `]`
# finds it: `${m[\]]}` is the key ], quoted or not, with an operator too
# (finding 376). tsh read nothing, the glob escape left on the key.
exec 2>&1
declare -A m; m[\]]=def; m['x]y']=xy
echo "1[${m[\]]}] 2[${m[x\]y]}]"; echo 3[${m[\]]}] 4[${m[x\]y]}]
echo "5[${m[\]]:-none}] 6[${m[x\]y]^^}]"
