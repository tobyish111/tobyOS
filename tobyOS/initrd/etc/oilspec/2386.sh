# http://landley.net/notes.html#12-06-2020
shopt -s extglob

touch abc\)d
echo ab+(c?d)

IFS=c ABC="c?d"
echo ab+($ABC)

ABC='*'
echo $ABC
