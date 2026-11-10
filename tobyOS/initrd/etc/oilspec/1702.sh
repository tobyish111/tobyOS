mkdir x
touch \
  'x/test.ifs.\.txt' \
  'x/test.ifs.*.txt'
v='*\*.txt'
argv.py x/unmatching.$v
