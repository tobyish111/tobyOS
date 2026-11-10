exec 2>&1
foo=FOO; x=foo; ref=x
echo "1 ref=$ref !ref=${!ref}"
typeset -n ref
echo "1b ref=$ref !ref=${!ref}"
typeset -n ref1=ref2
typeset -n ref2=ref1
echo "2 defined $?"
ref1=z
echo "2 mutated $?"
echo "2b [$ref1]" ; echo "2c s=$?"
typeset -n ra='a[@]'
a=('A B' C)
printf '<%s>' ra "$ra"; echo
ra=(X Y Z)
echo "3 status=$?"
printf '<%s>' 'ra[@]' "${ra[@]}"; echo
printf '<%s>' ra "$ra"; echo
printf '<%s>' 'a[@]' "${a[@]}"; echo
array=(X Y Z)
typeset -n rz='array[0]'
rz[0]=foo
echo "4 status=$?"
echo "4b ${array[@]}"
set -- A B
typeset -n rat='@'
echo "5 status=$?"
echo "5b rat=$rat"
echo "5c status=$?"
show() { echo show; printf '<%s>' ${!hooksSlice}; echo; printf '<%s>' ${!hooksSlice+"${!hooksSlice}"}; echo; }
hooksSlice='preHooks[@]'
preHooks=(); show
preHooks=('foo bar' baz); show
set -u
preHooks=(); show
preHooks=('foo bar' baz); show
set +u
role_post=_foo
orig() { export NIX_LDFLAGS${role_post}+=" -L$1/lib64"; }
orig one; declare -p NIX_LDFLAGS_foo
orig two; declare -p NIX_LDFLAGS_foo
typeset -n r6=nosuch; echo "6 [${!r6}] [${r6-unset}]"
typeset -n r7; echo "7 [${!r7}] s=$?"
r8=1; typeset -n r8; echo "8 [${!r8}] [$r8]"
