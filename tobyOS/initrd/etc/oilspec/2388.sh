# http://landley.net/notes.html#14-05-2020
shopt -s extglob

touch l; echo [hello"]"

touch b
echo [$(echo abc)]

touch +
echo [+()]
echo [+(])
