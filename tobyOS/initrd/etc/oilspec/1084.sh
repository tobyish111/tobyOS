case $SH in dash|zsh|ash) exit ;; esac

echo 'delim c'
echo '  a b c ' | (read -d 'c' -n 3; echo "[$REPLY]")
echo '  a b c ' | (read -d 'c' -n 4; echo "[$REPLY]")
echo '  a b c ' | (read -d 'c' -n 5; echo "[$REPLY]")
echo

echo 'one var'
echo '  a b c ' | (read -d 'c' -n 3 myvar; echo "[$myvar]")
echo '  a b c ' | (read -d 'c' -n 4 myvar; echo "[$myvar]")
echo '  a b c ' | (read -d 'c' -n 5 myvar; echo "[$myvar]")
echo

echo 'three vars'
echo '  a b c ' | (read -d 'c' -n 3 x y z; echo "[$x] [$y] [$z]")
echo '  a b c ' | (read -d 'c' -n 4 x y z; echo "[$x] [$y] [$z]")
echo '  a b c ' | (read -d 'c' -n 5 x y z; echo "[$x] [$y] [$z]")
