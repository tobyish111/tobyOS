# `declare NAME=value` on a readonly ARRAY is declare's refusal, "declare:
# a: readonly variable", as it is on a readonly scalar -- tsh gave the bare
# assignment message (finding 491). A plain assignment stays bare.
exec 2>&1
a=(x); readonly a
declare a=7; echo "rc=$?"
typeset a=8; echo "rc=$?"
s=1; readonly s
declare s=2; echo "rc=$?"
a=9; echo "rc=$?"
echo next
