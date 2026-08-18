hello=100
a=([hello]=1 [hello]+=2)
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
a+=([hello]+=:34 [hello]+=:56)
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
