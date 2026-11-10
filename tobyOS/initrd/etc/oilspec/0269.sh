case $SH in mksh) exit ;; esac

sp1=()
([[ -v sp1[-1] ]]; echo "$? (expect 1)")
sp2=({1..9})
([[ -v sp2[-10] ]]; echo "$? (expect 1)")
sp3=({1..9})
unset -v 'sp3[4]'
([[ -v sp3[-10] ]]; echo "$? (expect 1)")
