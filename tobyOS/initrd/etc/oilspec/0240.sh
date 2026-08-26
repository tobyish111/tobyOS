i=5
v='1 2 3'
declare -A a
a=([i]=$v)
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"

x=(3 5 7)
a=([i]="${x[*]}")
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
a=([i]="${x[@]}")
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
a=([i]=${x[*]})
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
a=([i]=${x[@]})
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
