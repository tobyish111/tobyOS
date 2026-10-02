# TILDE EXPANSION BY BASH'S RULES: a bare `~` is $HOME as it is -- HOME=''
# makes it empty, not "/" -- and only an UNSET HOME falls back to the
# user's passwd entry; `~+` and `~-` are PWD and OLDPWD; the home is glued
# on as it is (no slash dropped); and inside [[ ]] a leading tilde
# expands too, its result QUOTED (a `*` or `^` in HOME is literal).
exec 2>&1
h=$HOME
HOME=/home/bob
echo "a [$(echo ~)] [$(echo ~/x)]"
HOME=''
echo "b [$(echo ~)] [$(echo ~/x)]"
x=~; echo "c [$x]"
x=a:~:b; echo "d [$x]"
[[ ~ ]]; echo "e $?"
[[ -n ~ ]]; echo "f $?"
[[ ~ == ~ ]]; echo "g $?"
HOME='/h*'
[[ /hzz == ~ ]]; echo "h $?"
[[ '/h*' == ~ ]]; echo "i $?"
[[ ~ == '/h*' ]]; echo "j $?"
HOME='^a$'
[[ '^a$' =~ ~ ]]; echo "k $?"
[[ a =~ ~ ]]; echo "l $?"
HOME=/home/bob
[[ ~/x == /home/bob/x ]]; echo "m $?"
[[ "~" == /home/bob ]]; echo "n $?"
[[ \~ == /home/bob ]]; echo "o $?"
[[ ~"" == /home/bob ]]; echo "p $?"
[[ x~ == x/home/bob ]]; echo "q $?"
[[ ~root == /root ]]; echo "r $?"
[[ ~nosuchuser == '~nosuchuser' ]]; echo "s $?"
unset HOME
echo "t [$(echo ~)]"
[[ ~ == "$(echo ~)" ]]; echo "u $?"
HOME=$h
HOME=/
echo "v [$(echo ~/x)]"
HOME=/home/bob/
echo "w [$(echo ~/x)]"
cd /tmp; cd /
echo "x [$(echo ~+)] [$(echo ~-)] [$(echo ~+/a)]"
[[ ~+ == / ]]; echo "y $?"
[[ ~- == /tmp ]]; echo "z $?"
unset OLDPWD
echo "aa [$(echo ~-)]"
HOME=$h
