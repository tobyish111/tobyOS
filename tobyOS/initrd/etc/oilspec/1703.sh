mkdir x
touch \
  'x/test.ifs.\.txt' \
  'x/test.ifs.*.txt'

v='*\*.txt'
argv.py x/$v

v="\\" u='*.txt'
argv.py x/*$v$u

v="\\" u="*.txt"
argv.py x/*$v*.txt
