shopt -s extglob
mkdir -p eg10
cd eg10

touch {'a b c',bee,cee}.{py,cc}
set -- 'a b' 'c'

argv.py "$@"

# This works!
argv.py star glob "$*"*.py
argv.py star extglob "$*"*@(.py|cc)

# Hm this actually still works!  the first two parts are literal.  And then
# there's something like the simple_word_eval algorithm on the rest.  Gah.
argv.py at extglob "$@"*@(.py|cc)
