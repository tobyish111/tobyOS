# A quoted `'NAME=(...)'` that readonly or export refuses for a readonly
# name is the builtin's argument: status 1 and the line goes on -- only a
# list written as one, NAME=(...), ends it. tsh ended the line for both
# (finding 488).
exec 2>&1
a=(x); readonly a
readonly 'a=(3)'; echo "rc=$?"
export 'a=(4)'; echo "rc=$?"
f() { readonly 'a=(5)'; echo "in f $?"; }; f; echo "rc=$?"
readonly a=(6); echo "not reached"
echo next
