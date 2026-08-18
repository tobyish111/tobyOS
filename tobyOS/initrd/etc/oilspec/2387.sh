# http://landley.net/notes.html#17-05-2020

shopt -s extglob  # required for bash, not osh
IFS=x; ABC=cxd; for i in +($ABC); do echo =$i=; done
