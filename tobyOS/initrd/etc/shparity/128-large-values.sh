s=0123456789abcdef
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do s="$s$s"; done
h=${s:0:40000}
echo "v1 [${#s}]"
x=$(printf '%s' "$s"); echo "v2 [${#x}]"
x=`printf '%s' "$s"`; echo "v3 [${#x}]"
y="$x$x$x"; echo "v4 [${#y}]"
z=${y:196600}; echo "v5 [${#z}] [$z]"
w=${s//a/A}; echo "v6 [${#w}] [${w:0:16}]"
v=${s%?????}; echo "v7 [${#v}]"
u=${s#*f}; echo "v8 [${#u}]"
t=${s^^}; echo "v9 [${t:0:16}]"
printf '%s\n' "$s$s" | { read -r l; echo "v10 [${#l}]"; }
n=0; while read -r l; do n=$((n + ${#l})); done <<< "$h"; echo "v11 [$n]"
lst=; i=0; while [ $i -lt 1000 ]; do i=$((i+1)); lst="$lst $i"; done
set -- $lst; echo "v12 [$#] [$1] [${1000}] [${#lst}]"
a=(); a+=("$s"); echo "v13 [${#a[0]}]"
eval "big=\"$s\""; echo "v14 [${#big}]"
f() { local q; q=$(printf '%s' "$s$s"); echo "v15 [${#q}]"; }; f
case "$s" in *"${s:65530}") echo "v16 hit";; *) echo "v16 miss";; esac
[[ "$y" == *"$z" ]] && echo "v17 ok"
x=$(i=0; while [ $i -lt 5000 ]; do i=$((i+1)); echo $i; done); echo "v18 [${#x}]"
e=${x##*$'\n'}; echo "v19 [$e]"
code=""; i=0; while [ $i -lt 2500 ]; do i=$((i+1)); code="$code : $i;"; done
eval "g() { $code echo v20 function-ok; }"; g; echo "v20 [${#code}]"
prog="if true; then"$'\n'
i=0; while [ $i -lt 1500 ]; do i=$((i+1)); prog="$prog  : line $i"$'\n'; done
prog="$prog  echo v21 compound-ok"$'\n'"fi"
eval "$prog"; echo "v21 [${#prog}]"
mapfile -t m <<< "$h"; echo "v22 [${#m[@]}] [${#m[0]}]"
IFS= read -r -d '' hd <<EOF
$h
EOF
echo "v23 [${#hd}]"
k=${s:0:600}; case "$k" in "$k") echo "v24 long-pattern-ok";; esac
q=$(printf '%s%s' "$s" "$s"); echo "v25 [${#q}]"
for it in "$s"; do echo "v26 [${#it}]"; done
unset uv; : "${uv:=$s}"; echo "v27 [${#uv}]"
lit=L; for i in 1 2 3 4 5 6 7; do lit="$lit$lit"; done
bx=$(eval "printf '%s\n' ${lit}{1..80}"); echo "v31 [${#bx}] [${bx:0:3}]"
echo "$(seq 20000)" | tail -1 | sed "s/^/v32 /"
x=$(seq 30000); echo "$x" | wc -c | sed "s/^/v33 /"
echo done
