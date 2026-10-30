# With patsub_replacement on (bash 5.2's default), an unquoted & in the
# replacement of ${v/pat/rep} is the text the pattern matched; a quoted or
# backslash-escaped & is an &, \\ is a backslash, and with the option off
# every & is an & (finding 399).
exec 2>&1
v=hello; echo "${v/l/[&]}" "${v//l/<&&>}" ${v/#h/&&} ${v/%o/(&)}
echo "${v/l/'&'}" "${v/l/"&"}" "${v/l/\&}" ${v/l/\&}
echo "${v/l/\\&}" "${v/l/a\\b}"
r='<&>'; echo "${v/l/$r}" "${v/l/"$r"}"; r2='x\&y'; echo ${v/l/$r2}
a=(ab cb); echo "${a[@]/b/<&>}"; set -- xy zy; echo "${@/y/&&}"
w=abc; echo "${w/q/&}" "${w/#/<&>}" "${w/%/[&]}"
shopt -u patsub_replacement; echo "${v/l/[&]}" ${v//l/&&}
