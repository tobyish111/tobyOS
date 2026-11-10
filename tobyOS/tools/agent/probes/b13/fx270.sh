# printf's floating conversions as bash 5.2 makes them: the operand is a
# LONG DOUBLE (strtold), printed exactly and rounded half to even -- the
# binary value's digits at high precision, %.0f of 2.5 is 2 -- with %a and
# %A, inf and nan, hexadecimal operands, "invalid number" for what strtold
# leaves over and "Result not representable" for what does not fit; and
# the integer conversions' out-of-range warnings, strtoumax for %o %x %X,
# and "invalid octal/hex number". tsh carried the operand's decimal digits
# instead, had no %a, inf or nan, and printed 1e-310 as 1e-3310 (logs/b13
# q50 q53).
exec 2>&1
printf '%.30f\n' 0.1
printf '%.25f\n' 2.675
printf '%.20e\n' 1.1
printf '%.25g\n' 0.3
printf '%f\n' 1e300 | cut -c1-40
printf '%.3f|%g|%g\n' 1e-300 1e-310 1e-4940
printf '%.40f\n' 0.5
printf '%f\n' 123456789012345678901234567890
printf '%.0f|' 0.5 1.5 2.5 3.5; echo
printf '%.2f|' 0.125 0.375 2.675 99.995; echo
printf '%a|' 1 0.5 255 -2.5 0 0.1 1e4930; echo
printf '%A|%.3a|%.0a|%.1a|%.15a|%.20a\n' 10 3.14159 1.5 1.96875 0.1 0.1
printf '%#a|%#.0a|%10a|%-12a|%+a|% a|%015a\n' 1 1 1 1 1 1 1
printf '%e|%g|%g|%g|%g|%#g\n' 0 100000 1000000 0.0001 0.00001 1
printf '%#.2g|%#.0e|%#.0f\n' 99.995 1 1
printf '%f|' inf -inf INFINITY nan -nan; echo
printf '%010f|%-010f|%+010.3f\n' inf -1 nan
printf '%5.1f|%-8.2e|%+g|%08.2f|% f\n' 3.14159 31415.9 2 -1.5 1
printf '%f|%f\n' 0x10 0x1.8p1
printf '%.3f|%.3f\n' "'A" '"B'
printf '%f|' " 1.5" "1.5 " "+.5" "-." "1e" "1e+" "0x1.8p" "0x" "1,5" infx "(nan)"; echo
echo "status=$?"
printf '%f|%f\n' 1e5000 1e-5000
echo "status=$?"
printf '%d|%d\n' 9223372036854775807 9223372036854775808
printf '%d|%u|%x\n' -9223372036854775809 18446744073709551616 18446744073709551616
printf '%x|%X|%o|%#x|%#o\n' 18446744073709551615 9223372036854775808 18446744073709551615 0 0
echo "status=$?"
printf '%d|' 08 0x1g 0X1G 012 0x1F ' 7' '7 '; echo
echo "status=$?"
printf '%.40Lf|%lf|%hf|%Lg\n' 0.1 1.5 2.5 3.5
