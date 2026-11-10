exec 2>&1 </dev/null
printf '%.30f\n' 0.1
printf '%.25f\n' 2.675
printf '%.20e\n' 1.1
printf '%.25g\n' 0.3
printf '%f\n' 1e300
printf '%.3f\n' 1e-300
printf '%g\n' 1e-310
printf '%.40f\n' 0.5
printf '%f\n' 123456789012345678901234567890
printf '%.0f\n' 0.5 1.5 2.5 3.5
printf '%a\n' 1 0.5 255 -2.5 0
printf '%A\n' 10
printf '%.3a\n' 3.14159
printf '%e\n' 0
printf '%g\n' 100000 1000000 0.0001 0.00001
printf '%#g\n' 1
printf '%#.3x %#o\n' 255 8
printf '%f\n' inf -inf nan
printf '%5.1f|%-8.2e|%+g\n' 3.14159 31415.9 2
printf '%.60f\n' 1
printf '%.100g\n' 0.1
printf '%f\n' 0x10
printf '%f\n' 1e
echo "rc=$?"
printf '%d\n' 9223372036854775807 9223372036854775808 -9223372036854775809
echo "rc=$?"
printf '%u\n' -1
printf '%x\n' -1
printf '%i\n' 010 0x1f "'A"
printf '%c%c\n' hello ""
printf '%5s|%-5s|%.2s\n' ab cd efgh
printf '%b\n' 'a\tb' '\0101' '\x41' 'é' 'c\cd'
printf '%q\n' "a b" "" "'" $'\t'
printf '%(%Y)T\n' 0
printf '%s\n' "${PIPESTATUS[@]}"
echo end
