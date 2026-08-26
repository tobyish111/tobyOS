touch -- foo.txt -foo.txt

echo *t

GLOBIGNORE=*.txt
echo *t

shopt -s nullglob
echo nullglob *t
