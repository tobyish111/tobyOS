# POSIX 2.6.1 -- tilde expansion. Only at the START of a word, and only when
# unquoted; anywhere else a tilde is an ordinary character.
HOME=/homedir
echo ~
echo ~/sub
echo ~/a/b
echo "~"
echo '~'
echo a~b
echo /lead~
echo "quoted ~ inside"
x=~
echo "assign=$x"
y=~/tail
echo "assign2=$y"
count() { echo "n=$# 1=[$1]"; }
count ~
count ~/x
count "~"
HOME=/other
echo ~
echo end
