# In a here-document bash joins "$*"-style expansions by FORM (finding
# 302): $*, its slices, #/% and @ transforms -- and those of ${a[*]}, and
# ${!a[*]} -- with a space whatever IFS is; a pattern substitution or case
# change of $* or ${a[*]}, and ${!p*}, with IFS's first character unless
# the body holds an "@" expansion; a plain ${a[*]} with IFS's always. tsh
# used IFS's for all of them.
set -- Ab cD; a=(x Y); pa=1 pb=2; IFS=:
cat <<EOF
1 <$*> <${*}> <"$*"> <${*:1}> <${*#A}> <${*%D}> <${*@Q}> <${*@U}>
2 <${a[*]}> <${a[*]:0}> <${a[*]#x}> <${a[*]@Q}> <${!a[*]}>
3 <${*/b/B}> <${*^^}> <${*~}> <${!p*}> <${a[*]/x/q}> <${a[*],,}>
4 <${u-$*}> <${u-"$*"}>
EOF
cat <<EOF
5 <${*/b/B}> <${*^^}> <${!p*}> <${a[*]/x/q}> <${a[*]}> <$@>
EOF
IFS=
cat <<EOF
6 <$*> <${a[*]}> <${*:1}> <${a[*]/x/q}> <$@>
EOF
f() { cat <<EOF
7 <$*> <${*:2}>
EOF
}
IFS=: f p q r
cat <<< "$*"
echo "<$*> <${a[*]:0}>"
