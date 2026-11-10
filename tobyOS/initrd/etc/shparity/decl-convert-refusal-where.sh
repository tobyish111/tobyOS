# Over an array of the other kind, a declaration's list is refused as bash
# expands the command's words, and where it runs says what happens next:
# outside a function the rest of the line goes -- an eval's text, which
# then returns 1 -- inside one declare, typeset and local go on, and when
# -A (or -a) is one of their own options they refuse the word again in
# their own name, status 1; readonly's and export's refusal drops the line
# wherever it runs. tsh dropped the line only when no command name was
# set, and made an -a list into an associative array (finding 623).
exec 2>&1
aaa() { declare -g -a unique=(); }
bbb() { declare -g -A unique=(); }
aaa; bbb; echo "after bbb: $?"
u=(1 2)
g() { :; declare -g -A u=([a]=x); echo "in g rc=$?"; }
g; echo "g rc=$?"; declare -p u
eval 'declare -A u=([k]=x); echo "not reached"'; echo "eval rc=$?"
h() { readonly -A u=([a]=x); echo "not reached"; }
h; echo "not reached either"
echo "after h rc=$?"
l() { local -a w=(1); local -A w=([a]=x); echo "in l rc=$?"; declare -p w; }
l
declare -A A=([a]=x)
m() { declare -g v -a A=(1 2); echo "in m rc=$?"; }
m; declare -p A
n() { export -a A=(1 2); echo "not reached"; }
n; echo "not reached either"
echo "after n rc=$?"; declare -p A
readonly -a A=(1 2); echo "not reached"
declare -p A
