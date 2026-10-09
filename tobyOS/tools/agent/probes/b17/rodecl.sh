# rodecl.sh: a compound assignment to a READONLY name through a declaration
# builtin (findings 10 26). bash declares the name internally first, with
# the builtin's -a/-A/-g and transforming options, and that declare reports
# under the LAST simple command's name; in a function local/declare/typeset
# then run on, elsewhere the command is discarded. The script's path and
# line are taken out.
exec 2>&1
N() { sed 's/^[^:]*: line [0-9]*: //'; }
t() { $SH -c "$1" 2>&1 | N; echo "rc=${PIPESTATUS[0]}"; }
t 'readonly r=1; true; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; true; declare r=(9); echo "st=$?"'
t 'readonly r=1; true; declare -A r=([k]=9); echo "st=$?"'
t 'readonly r=1; true; declare -i r=(9); echo "st=$?"'
t 'readonly r=1; true; typeset -a r=(9); echo "st=$?"'
t 'readonly r=1; true; export r=(9); echo "st=$?"'
t 'readonly r=1; true; readonly r=(9); echo "st=$?"'
t 'readonly r=1; true; declare -a r=(9) q=2; echo "st=$? q=$q"'
t 'readonly r=1; x=5; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; echo -n; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; f() { local -a r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { local r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { declare -a r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { true; declare -a r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { local -a r=(9) s=(1); echo "in f st=$? s=${s[*]}"; }; f'
t 'readonly r=1; f() { export r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { declare -ga r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly -a r=(1); true; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; true; declare -a r+=(9); echo "st=$?"'
t 'readonly r=1; /bin/true; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; g() { :; }; g; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; true | true; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; (true); declare -a r=(9); echo "st=$?"'
t 'readonly r=1; declare -a r=(9); echo "st=$?"'
t 'readonly r=1; f() { (( 1 )); local -a r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { eval "local -a r=(9)"; echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { readonly r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; f() { /bin/true; local -a r=(9); echo "in f st=$?"; }; f; echo "st=$?"'
t 'readonly r=1; g() { :; }; f() { g; local -a r=(9); echo "in f st=$?"; }; f'
t 'readonly r=1; f() { local r=9; echo "in f st=$?"; }; f'
echo end
