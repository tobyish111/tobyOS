mkdir x
touch \
  x/test.ifs.\\.txt \
  x/test.ifs.\'.txt \
  x/test.ifs.a.txt \
  x/test.ifs.\\b.txt

v="*\\*.txt"
argv.py x/$v

v="*\'.txt"
argv.py x/$v

v='*\a.txt'
argv.py x/$v

v='*\b.txt'
argv.py x/$v


# 3 shells treat \ in unquoted substitution $v as literal \
