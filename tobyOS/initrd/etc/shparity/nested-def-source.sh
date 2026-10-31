# A function defined while another one runs is from that one's file: its
# BASH_SOURCE and its diagnostics name the sourced file the outer function
# came from -- tsh named the script (finding 530).
exec 2>&1
echo 'mk() { made() { echo "${BASH_SOURCE[0]}"; nosuchcmd_530; }; }' > n.sub
. ./n.sub
mk
made
