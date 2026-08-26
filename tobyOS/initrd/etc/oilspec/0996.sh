printf '%d\n' 42
printf '%i\n' 42  # synonym
printf '%d\n' \'a # if first character is a quote, use character code
printf '%d\n' \"a # double quotes work too
printf '[%5d]\n' 42
printf '[%-5d]\n' 42
printf '[%05d]\n' 42
#printf '[%-05d]\n' 42  # the leading 0 is meaningless
#[42   ]
