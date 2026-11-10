# [[ ]]: operators that need no blanks, "$@" as one word, a leading tilde,
# and a redirection on the command itself.
[[ b>a ]] && echo t1
[[ b<a ]] || echo t2
[[ a<b&&b<c ]] && echo t3
[[ ''||! (1 == 2)&&(2 == 2)]] && echo t4
[[ x||'' ]] && echo t5
[[ (a)&&(b) ]] && echo t6
[[ ! ''&&x ]] && echo t7
[[ a == a||b == c ]] && echo t8
x='a b'
[[ $x == 'a b'&&-n $x ]] && echo t9
[[ "&&" ]] && echo t10
[[ '<' < '>' ]] && echo t11
[[ \< < \> ]] && echo t12
[[ 1 -lt 2&&2 -lt 3 ]] && echo t13
[[ ab =~ a|c ]] && echo t14
[[ cb =~ (a|c)b ]] && echo t15
[[ x =~ x&&y =~ y ]] && echo t16
shopt -s extglob
[[ ab == @(ab|cd) ]] && echo t17
[[ cd == @(ab|cd)&&1 ]] && echo t18
shopt -u extglob
echo ---
set -- 1 '3 5'
a=('1 3' 5)
b=(1 2 3)
[[ "$@" = "${a[@]}" ]] && echo d1
[[ "$@" = "${b[@]}" ]] || echo d2
[[ '1 3 5' = "${a[@]}" ]] && echo d3
[[ "${a[*]}" = "$*" ]] && echo d4
[[ "x$@y" == 'x1 3 5y' ]] && echo d5
[[ $@ == '1 3 5' ]] && echo d6
set --
[[ "$@" ]] || echo d7
[[ -z "$@" ]] && echo d8
e=()
[[ "${e[@]}" == '' ]] && echo d9
IFS=:
set -- a b
[[ "$*" == a:b && "$@" == 'a b' ]] && echo d10
IFS=$' \t\n'
echo ---
HOME=/home/bob
[[ ~ == /home/bob ]] && echo h1
[[ ~/x == /home/bob/x ]] && echo h2
[[ ~ == /home/b* ]] && echo h3
[[ /home/bob == ~ ]] && echo h4
[[ "~" == '~' ]] && echo h5
[[ x~ == 'x~' ]] && echo h6
[[ ~root == /root ]] && echo h7
[[ ~nouser == '~nouser' ]] && echo h8
HOME='/h*'
[[ /hx == ~ ]] || echo h9
[[ '/h*' == ~ ]] && echo h10
HOME='^a$'
[[ '^a$' =~ ~ ]] && echo h11
[[ a =~ ~ ]] || echo h12
HOME=''
[[ ~ ]] || echo h13
[[ -z ~ ]] && echo h14
HOME=/
echo ---
d=/tmp/dbrt; rm -rf $d; mkdir -p $d
[[ $(echo out; echo err >&2) == out ]] 2>$d/e && echo r1
cat $d/e
[[ -n $(echo x >&2) ]] 2>/dev/null || echo r2
[[ x ]] > $d/o; echo "r3 $? [$(cat $d/o)]"
[[ $(cat) == fed ]] < <(echo fed) && echo r4
[[ x ]] > /nonexistent/dir/f; echo "r5 $?"
rm -rf $d
