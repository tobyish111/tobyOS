case $SH in dash|zsh) exit ;; esac

echo '  a b  ' | (read -n 4; echo "[$REPLY]")
echo '  a b  ' | (read -n 5; echo "[$REPLY]")
echo '  a b  ' | (read -n 6; echo "[$REPLY]")
echo

echo 'one var strips whitespace'
echo '  a b  ' | (read -n 4 myvar; echo "[$myvar]")
echo '  a b  ' | (read -n 5 myvar; echo "[$myvar]")
echo '  a b  ' | (read -n 6 myvar; echo "[$myvar]")
echo

echo 'three vars'
echo '  a b  ' | (read -n 4 x y z; echo "[$x] [$y] [$z]")
echo '  a b  ' | (read -n 5 x y z; echo "[$x] [$y] [$z]")
echo '  a b  ' | (read -n 6 x y z; echo "[$x] [$y] [$z]")
