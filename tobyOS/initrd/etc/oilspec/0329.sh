files=('foo.c' 'sp ace.h' 'bar.c')
argv.py "${files[@]%.c}"
