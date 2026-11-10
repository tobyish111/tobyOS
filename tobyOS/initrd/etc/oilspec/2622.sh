# quotes are LITERAL here
argv.py "${undef-'c d'}" "${undef-'c  d'}"
argv.py ${undef-'c d'} ${undef-'c  d'}

echo ---

# quotes are RESPECTED here
foo='a b c d'
argv.py "${foo%'c d'}" "${foo%'c  d'}"

case $SH in dash) exit ;; esac

argv.py "${foo//'c d'/zzz}" "${foo//'c  d'/zzz}"
argv.py "${foo//'c d'/'zzz'}" "${foo//'c  d'/'zzz'}"
