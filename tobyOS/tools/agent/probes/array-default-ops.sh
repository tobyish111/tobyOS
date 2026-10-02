# ${a[@]OP word} and ${a[*]OP word} for OP in - :- + :+ = :=  ? :? -- the
# test is on the WHOLE array, not element by element.
show() { printf '%s' "$1:"; shift; printf ' <%s>' "$@"; echo; }
v=(1 2 3); e=(); n=(''); nn=('' ''); unset u
show a1 "${v[@]-d}" "${v[@]:-d}" "${v[*]:-d}"
show a2 "${e[@]-d}" "${e[@]:-d}" "${e[*]-d}" "${e[*]:-d}"
show a3 "${n[@]-d}" "${n[@]:-d}" "${n[*]-d}" "${n[*]:-d}"
show a4 "${nn[@]-d}" "${nn[@]:-d}" "${nn[*]-d}" "${nn[*]:-d}"
show a5 "${u[@]-d}" "${u[@]:-d}" "${u[*]-d}" "${u[*]:-d}"
show a6 "${v[@]+p}" "${v[@]:+p}" "${e[@]+p}" "${e[@]:+p}" "${n[@]+p}" "${n[@]:+p}" "${u[@]+p}"
show a7 ${v[@]:-d} ${e[@]:-d e} ${u[*]-x y}
show a8 "${v[@]:-9}" "${v[@]: -1}" "${v[@]:1}"
IFS=:
show a9 "${nn[*]:-d}" "${nn[*]-d}"
IFS=$' \t\n'
s=str; unset t
show a10 "${s[@]:-d}" "${t[@]:-d}" "${s[*]+p}"
(echo "${u[@]:?gone}"; echo after) 2>&1 | sed 's/^.*line [0-9]*: //'
(echo "${e[@]?gone}"; echo after2) 2>&1 | sed 's/^.*line [0-9]*: //'
(echo "${u[@]=x}"; echo after3) 2>&1 | sed 's/^.*line [0-9]*: //'
(echo "${u[*]:=x}"; echo after4 "${u[*]}") 2>&1 | sed 's/^.*line [0-9]*: //'
echo end
