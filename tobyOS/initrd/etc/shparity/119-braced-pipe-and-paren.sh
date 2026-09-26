t='x|y'
v=$'a\nb'
echo B1 ${t//|/-}
k=${t//|/-}; echo "B2 [$k]"
k=${v//$'\n'/-}; echo "B3 [$k]"
k=${v//$'\n'/|}; echo "B4 [$k]"
echo B5 ${v//$'\n'/|}
k=${t//$'|'/-}; echo "B6 [$k]"
k=${t//x/$'a'}; echo "B7 [$k]"
u='a(b'; echo B8 ${u//(/x} | tr a-z A-Z
unset n; echo B9 ${n:-a|b}
s='x;y&z'; echo B10 ${s//;/-} ${s//&/+}
echo B11 ${v//$'\n'/_} | tr a-z A-Z
k=${t//|/$'\t'}; echo "B12 ${#k}"
echo done
