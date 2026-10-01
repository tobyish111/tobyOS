echo "top: $$ $BASHPID"
{ echo "stage: $$ $BASHPID"; } | cat
{ echo "bg: $$ $BASHPID"; } & wait
echo "cmdsub: $(echo $$ $BASHPID)"
( echo "sub: $$ $BASHPID" )
sh -c 'echo "child sh: $$"'
