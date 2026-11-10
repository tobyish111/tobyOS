# A compound assignment's list ends at its own `)`: not an escaped one, nor
# one inside quotes -- in a declaration's argument and through eval too
# (finding 364). tsh ended the list at the inner `)`, and the rest ran as a
# command or was "unmatched double quote".
exec 2>&1
typeset -A B=( [\\]=bs [\"]=dq [\)]=rp ); declare -p B
declare -A F=([")"]="rp" ["\""]="dq" ["\\"]="bs"); declare -p F
eval 'declare -A G=([")"]="rp" ["\""]="dq")'; declare -p G
a=( "x)y" 'p)q' z\)w ); declare -p a
