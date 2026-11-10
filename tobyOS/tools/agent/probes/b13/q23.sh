S=${SH:-bash}; S=$(command -v $S)
F='^(BASH_ARGC|BASH_ARGV|BASH_LINENO|BASH_SOURCE|DIRSTACK|GROUPS|FUNCNAME|BASH_ALIASES|BASH_CMDS|BASH_REMATCH|PIPESTATUS|COMP_WORDBREAKS|BASH_COMMAND|BASH_SUBSHELL|BASHPID|EPOCH|LINENO|RANDOM|SECONDS|SRANDOM|HISTCMD)='
echo "== c"; env -i $S -c "set | grep -E '$F'"
echo "== c-read"; env -i $S -c ": \${DIRSTACK[0]} \${GROUPS[0]}; set | grep -E '$F'"
printf 'set | grep -E "%s"\n' "$F" > /tmp/q23s.sh
echo "== script"; env -i $S /tmp/q23s.sh
echo "== script-args"; env -i $S /tmp/q23s.sh a b
printf 'f() { set | grep -E "%s"; }; f x y\n' "$F" > /tmp/q23f.sh
echo "== func"; env -i $S /tmp/q23f.sh
echo "== declare-p"; env -i $S -c 'declare -p BASH_LINENO BASH_SOURCE BASH_ARGC BASH_ARGV FUNCNAME DIRSTACK GROUPS 2>&1'
echo end
