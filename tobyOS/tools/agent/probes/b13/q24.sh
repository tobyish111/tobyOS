S=${SH:-bash}; S=$(command -v $S)
env -i $S -c 'set > /tmp/s1; grep "^BASH_ARGC\|^BASH_ARGV" /tmp/s1; (declare -p BASH_ARGC BASH_ARGV); x=$(declare -p BASH_ARGC); echo "$x"; declare -p BASH_ARGC | cat; f() { declare -p BASH_ARGC BASH_ARGV; }; f a b; declare -p BASH_ARGC'
printf 'declare -p BASH_ARGC BASH_ARGV\nf() { declare -p BASH_ARGC BASH_ARGV; }; f p q r\n. /tmp/q24b.sh x y\n' > /tmp/q24a.sh
printf 'declare -p BASH_ARGC BASH_ARGV\n' > /tmp/q24b.sh
echo ==; env -i $S /tmp/q24a.sh 1 2
echo end
