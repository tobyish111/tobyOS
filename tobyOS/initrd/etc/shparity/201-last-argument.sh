# $_ is the last word of the previous simple command. tsh had no $_ at all.
exec 2>&1
echo a b c; echo "1 $_"
f() { :; }; f x y; echo "2 $_"
true "last arg"; echo "3 $_"
a=(p q); echo "4 [$_]"
(( 1 )); echo "6 $_"
