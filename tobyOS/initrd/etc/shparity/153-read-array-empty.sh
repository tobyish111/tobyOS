echo '' | (read -a a; echo "1 ${#a[@]}")
echo '' | (IFS=x read -a a; echo "2 ${#a[@]}")
echo '' | (IFS= read -a a; echo "3 ${#a[@]}")
echo 'a b' | (IFS= read -a a; echo "4 ${#a[@]} [${a[0]}]")
echo '  ' | (IFS= read -a a; echo "5 ${#a[@]} [${a[0]}]")
printf 'x' | (IFS= read -a a; echo "6 ${#a[@]} [${a[0]}]")
IFS=
echo '' | (read -a a; sh -c 'echo "7 $#"' _ "${a[@]}")
echo '' | (read -a a; echo "8 ${#a[@]}"; for w in "${a[@]}"; do echo "8w [$w]"; done)
unset IFS
echo '' | (read -a a; sh -c 'echo "9 $#"' _ "${a[@]}")
echo done
