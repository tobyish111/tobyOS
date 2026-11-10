case $SH in mksh) exit ;; esac

sp1=()
[[ -v sp1[0] ]]; echo "$? (expect 1)"
[[ -v sp1[9] ]]; echo "$? (expect 1)"

sp2=({1..9})
[[ -v sp2[0] ]]; echo "$? (expect 0)"
[[ -v sp2[8] ]]; echo "$? (expect 0)"
[[ -v sp2[9] ]]; echo "$? (expect 1)"
[[ -v sp2[-1] ]]; echo "$? (expect 0)"
[[ -v sp2[-2] ]]; echo "$? (expect 0)"
[[ -v sp2[-9] ]]; echo "$? (expect 0)"

sp3=({1..9})
unset -v 'sp3[4]'
[[ -v sp3[3] ]]; echo "$? (expect 0)"
[[ -v sp3[4] ]]; echo "$? (expect 1)"
[[ -v sp3[5] ]]; echo "$? (expect 0)"
[[ -v sp3[-1] ]]; echo "$? (expect 0)"
[[ -v sp3[-4] ]]; echo "$? (expect 0)"
[[ -v sp3[-5] ]]; echo "$? (expect 1)"
[[ -v sp3[-6] ]]; echo "$? (expect 0)"
[[ -v sp3[-9] ]]; echo "$? (expect 0)"
