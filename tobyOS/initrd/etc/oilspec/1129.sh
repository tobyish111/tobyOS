case $SH in mksh|ash|dash|zsh) exit 99; esac
IFS='x '
echo 'a b'     | (read -a a; argv.py "${a[@]}")
echo 'a b '    | (read -a a; argv.py "${a[@]}")
echo 'a bx'    | (read -a a; argv.py "${a[@]}")
echo 'a bx '   | (read -a a; argv.py "${a[@]}")
echo 'a b x'   | (read -a a; argv.py "${a[@]}")
echo 'a b x '  | (read -a a; argv.py "${a[@]}")
echo 'a b x x' | (read -a a; argv.py "${a[@]}")
