exec 2>&1 </dev/null
PYTHONHOME=/ PYTHONPATH=/lib/python310.zip python3 -S -B -c '
import os
r,w=os.pipe()
try:
  print("lseek pipe:", os.lseek(r,0,1))
except OSError as e:
  print("lseek pipe:", e)
os.write(w,b"abc\n")
try:
  print("lseek pipe after write:", os.lseek(r,0,1))
except OSError as e:
  print("lseek pipe after write:", e)
'
printf 'read v\nhello\necho "v=$v"\n' | bash
echo "rc=$?"
printf 'read v\nhello\necho "v=$v"\n' > /tmp/q58.in; bash < /tmp/q58.in; rm -f /tmp/q58.in
echo end
