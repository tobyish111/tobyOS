$SH -c 'trap "> zz" EXIT'
wc -l zz  # should exist
