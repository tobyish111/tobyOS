show_hex() { od -A n -t c -t x1; }

# NOTE: LANG is set to utf-8.
# ? is a glob that stands for one character

v='μ-'
echo ${v#?} | show_hex
echo
echo ${v##?} | show_hex
echo

v='-μ'
echo ${v%?} | show_hex
echo
echo ${v%%?} | show_hex
  \n
  0a

  \n
  0a

  \n
  0a

  \n
  0a
