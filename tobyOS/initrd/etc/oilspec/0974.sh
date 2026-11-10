cd () { echo "hi"; }
cd
builtin cd / && pwd
unset -f cd
