# A "$@" with no parameters inside the word of ${x+WORD}: its quotes are
# no quoted null, a quoted null beside it is one, inside double quotes the
# substitution is the empty string, "${@+x}" is "$@", and the word's first
# field joins a "$@" before it (bash's quote4.sub; finding 297).
n() { echo "$# $(printf '<%s>' "$@")"; }
set --; x=x; e=()
n ${x+"$@"''}
n ${x+''"$@"}
n ${x+"$@"""}
n ${x+"$@" "$@"}
n ${x+a "$@"}
n ${u-"$@" "$@"} ${u-c "$@"}
n "${x+"$@"}" "${u-"$@"}" "${x:+"$@"}"
n "$@${x+"$@"}" "${x+"$@"}$@"
n "${@+x}" "${@:+x}" "${@+x}"''
n ${x+"${e[@]}"''} "${x+"${e[@]}"}"
a=(${x+"$@"''}); b=(${x+"$@" "$@"}); echo "${#a[@]} ${#b[@]}"
for i in ${x+"$@" "$@"} ${x+b "$@"}; do printf '<%s>' "$i"; done; echo
IFS=:; n ${x+"$@":"$@"} ${x+"$@"''}; IFS=; n ${x+"$@" "$@"}; unset IFS
set -- p q
n "$@${x+"$@"}"
n "$@"${x+"$@"}
n "${x+"$@"}${x+"$@"}"
n ${x+"${e[@]}" "$@"}
set -- ''
n ${x+"$@" "$@"} . ${x+"$@"''} . "${@:+x}"
