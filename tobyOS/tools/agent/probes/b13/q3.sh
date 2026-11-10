alias ll='ls -l' gg='grep -n'
echo "a1=[${BASH_ALIASES[ll]}] n=${#BASH_ALIASES[@]}"
declare -p BASH_ALIASES
BASH_ALIASES[hi]='echo hello'
alias hi
unset 'BASH_ALIASES[gg]'
alias gg 2>&1; echo "s=$?"
echo "keys=[${!BASH_ALIASES[*]}]"
unalias ll; echo "after unalias n=${#BASH_ALIASES[@]} [${BASH_ALIASES[ll]-gone}]"
hash -r
hash cat 2>&1; echo "c1=[${BASH_CMDS[cat]}]"
BASH_CMDS[foo]=/bin/echo
hash -t foo; hash | sed 's/^ *[0-9]* *//' | sort
foo through-hash
unset 'BASH_CMDS[foo]'; hash -t foo 2>&1; echo "s=$?"
declare -p BASH_CMDS
hash -r; echo "after -r n=${#BASH_CMDS[@]}"
BASH_ALIASES=(); echo "reset n=${#BASH_ALIASES[@]}"; alias | wc -l
unset BASH_ALIASES; alias zz=1; echo "unset then [${BASH_ALIASES[zz]-none}]"
