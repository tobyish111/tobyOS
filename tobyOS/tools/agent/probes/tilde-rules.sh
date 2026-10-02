# Tilde expansion: what HOME='' and an unset HOME give, in plain words, in
# assignments and inside [[ ]]; the result is QUOTED (a pattern or regex
# character in HOME is literal).
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
[[ ~ == /root ]]; echo "u $?"
HOME=$h
# the home is glued on as it is: no slash is dropped
HOME=/
echo "v [$(echo ~/x)]"
HOME=/home/bob/
echo "w [$(echo ~/x)]"
# ~+ and ~- are PWD and OLDPWD
cd /tmp; cd /
echo "x [$(echo ~+)] [$(echo ~-)] [$(echo ~+/a)]"
[[ ~+ == / ]]; echo "y $?"
[[ ~- == /tmp ]]; echo "z $?"
unset OLDPWD
echo "aa [$(echo ~-)]"
HOME=$h
