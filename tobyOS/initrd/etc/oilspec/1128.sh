case $SH in mksh|ash|dash|zsh) exit 99; esac
echo '' | (read -a a; argv.py "${a[@]}")
IFS=x
echo '' | (read -a a; argv.py "${a[@]}")
IFS=
echo '' | (read -a a; argv.py "${a[@]}")
