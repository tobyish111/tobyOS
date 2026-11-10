var=$'\n'
argv.py "${var#?}"
argv.py "${var%''}"
argv.py "${var%"${var#?}"}"
var='a'
argv.py "${var#?}"
argv.py "${var%''}"
argv.py "${var%"${var#?}"}"
