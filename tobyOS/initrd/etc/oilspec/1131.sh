case $SH in mksh|ash|dash|zsh) exit 99; esac
IFS='x '
echo 'a x b'   | (read -a a; argv.py "${a[@]}")
echo 'a xx b'  | (read -a a; argv.py "${a[@]}")
echo 'a xxx b' | (read -a a; argv.py "${a[@]}")
echo 'a x xb'  | (read -a a; argv.py "${a[@]}")
echo 'a x x b' | (read -a a; argv.py "${a[@]}")
echo 'ax b'    | (read -a a; argv.py "${a[@]}")
echo 'ax xb'   | (read -a a; argv.py "${a[@]}")
echo 'ax  xb'  | (read -a a; argv.py "${a[@]}")
echo 'ax x xb' | (read -a a; argv.py "${a[@]}")
