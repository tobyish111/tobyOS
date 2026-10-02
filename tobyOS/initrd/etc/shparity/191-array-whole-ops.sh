# ${a[@]OP word} / ${a[*]OP word}, OP one of - = ? + with or without the colon,
# test the WHOLE array, as bash's parameter_brace_expand does: set when it
# has an element, null when the elements join to nothing. tsh read `:-`
# `:+` `:=` `:?` as a SLICE (`${a[@]:+foo}` under set -u evaluated `+foo`)
# and distributed the rest over the elements, so an empty or unset array
# never took its default.
exec 2>&1
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
