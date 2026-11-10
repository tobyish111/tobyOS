umask 0111
# spaces are an error in bash
# dash & mksh only interpret the first one
umask u=, g+, o-
if test $? -ne 0; then
  echo ok
fi
umask | tail -c 4
