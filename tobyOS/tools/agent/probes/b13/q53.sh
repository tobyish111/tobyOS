exec 2>&1 </dev/null
printf '%a|' 1 2 3 0.1 1e-4940 1e4930 -0 0.5 1.5 255.5; echo
printf '%.0a|%.1a|%.2a|%.15a|%.16a|%.20a\n' 1.5 1.96875 3.14159 0.1 0.1 0.1
printf '%#a|%#.0a|%10a|%-12a|%+a|% a|%015a\n' 1 1 1 1 1 1 1
printf '%A|%.3A\n' 0.1 255
printf '%#.2g|%#.2G|%#g|%#.0e|%#.0f\n' 99.995 99.995 0 1 1
printf '%g|%g|%g|%g\n' 1e-5 123456 1234567 0.0001234
printf '%.0e|%.0f|%.0g\n' 2.5 2.5 2.5
printf '%f|%f\n' 1e5000 1e-5000
echo "rc=$?"
printf '%f|%e\n' 0x1p-16445 0x1p-16446
printf '%f|' inf INF Infinity -nan nan '(nan)' infx; echo
echo "rc=$?"
printf '%5.1f|%-8.3e|%08.2f|%+.1e|% f\n' -3.14159 2 -1.5 0 1
printf '%010f|%-010f|%+010.3f\n' inf -1 nan
printf '%.3f\n' "'A" '"B'
printf '%f\n' " 1.5" "1.5 " "+.5" "-." "1e+" "0x1.8p" "0x" "1,5"
echo "rc=$?"
printf '%x %X %o %#x %#o\n' 18446744073709551615 9223372036854775808 18446744073709551615 0 0
printf '%u %d %x\n' 18446744073709551616 -9223372036854775809 -1
echo "rc=$?"
printf '%.40Lf\n' 0.1
printf '%lf|%hf|%Lg\n' 1.5 2.5 3.5
echo end
