# A backslash before a control byte in a case pattern that comes from an
# unquoted variable quotes that byte: with c=$'\1' and p="\\$c", `case $c in
# ($p)` matches -- tsh compared the backslash with the byte's escape mark
# (finding 436).
exec 2>&1
for c in $'\1' $'\2' $'\5' $'\177' x; do
	p="\\$c"; case "$c" in ( $p ) printf ok ;; ( * ) printf fail ;; esac
	case "${c}y" in ( ${p}y ) printf ok ;; ( * ) printf fail ;; esac
	echo
done
x=$'\\a\\b\\c\\\001\\d\\e\\f'; case $'abc\001def' in $x) echo ok1 ;; *) echo oops1;; esac
y='\a\b\c\\d\e\f'; z=$'abc\001def'; case $z in $y) echo ok2 ;; *) echo oops2;; esac
