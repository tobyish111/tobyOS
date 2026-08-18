# NOTE: bash 4.3 had a bug where it ignored the bad subscript, but now it is
# fixed.
a=('123' '456')
argv.py "${a[0]}" "${a[0][0]}"
