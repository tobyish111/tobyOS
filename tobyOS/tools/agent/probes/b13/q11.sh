exec 2>&1
alias zb=1 ab=2 mm=3; unalias ab; alias b2=4
alias
alias -p | head -2
unset DIRSTACK; pushd /tmp >/dev/null; echo "ds=[${DIRSTACK[*]}]"; popd >/dev/null
DIRSTACK=(x y); echo "ds2=[${DIRSTACK[*]}]"; dirs
echo end
