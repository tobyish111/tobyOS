trap 'echo assign' ERR
a=$(false) | a=$(false) | a=$(false)

trap 'echo dparen' ERR
(( 0 )) | (( 0 )) | (( 0 ))

trap 'echo dbracket' ERR
[[ a = b ]] | [[ a = b ]] | [[ a = b ]]

# bash anomaly - it gets printed twice?
trap 'echo subshell' ERR
(false) | (false) | (false) | (false)

# same bug
trap 'echo subshell2' ERR 
(false) | (false) | (false) | (false; false)

trap 'echo group' ERR
{ false; } | { false; } | { false; }

echo ok
