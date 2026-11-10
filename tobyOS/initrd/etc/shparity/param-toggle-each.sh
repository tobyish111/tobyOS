# ${*~} and ${@~} toggle the case of EACH positional parameter's first
# character, as ${a[*]~} does each element's: tsh toggled the joined text's
# first character for ${*~} and nothing for ${@~} (finding 307).
set -- ab cD 'e f'
printf '<%s>' "${*~}" "${@~}" ${*~} ${@~~}; echo
IFS=:; printf '<%s>' "${*~}" "${*~~}"; echo; unset IFS
f() { printf '<%s>' "${@~}"; echo; }; f Xy zW
a=(ab cD); printf '<%s>' "${a[*]~}" "${a[@]~}"; echo
