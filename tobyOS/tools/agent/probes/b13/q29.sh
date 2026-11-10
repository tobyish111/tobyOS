exec 2>&1 </dev/null
cd /tmp
n=0
run() {
  n=$((n+1))
  printf '%s\n' "set -o history -H" "$@" > /tmp/q29_$n.sh
  echo "== case $n"
  $SH /tmp/q29_$n.sh
  echo "rc=$?"
}
run 'echo hi' 'echo !!' 'echo "!!"' "echo '!!'" 'echo \!!' 'echo x!!y'
run 'echo a' 'echo b' 'echo !1' 'echo !-2' 'history'
run 'echo alpha' 'true beta' 'echo !ec' 'echo !?lph?' 'echo !?bet' 'echo !t'
run 'echo one two three' 'echo !1:1' 'echo !1:$' 'echo !1:^' 'echo !1:*' 'echo !1:1-2' 'echo !1:2*' 'echo !1:1-' 'echo !1:-2' 'echo !1:0'
run 'echo one two three' 'echo !$' 'echo !^' 'echo !*' 'echo !:1' 'echo !-2:$'
run 'echo /usr/lib/x.tar.gz' 'echo !1:1:h' 'echo !1:1:t' 'echo !1:1:r' 'echo !1:1:e' 'echo !1:1:r:r' 'echo !1:1:t:r'
run 'echo hello world' 'echo !!:p' 'echo after' 'history'
run "echo it's" 'echo !1:1:q' 'echo a b' 'echo !3:x' 'echo !3:q'
run 'echo aaa bab' 'echo !!:s/a/X/' 'echo aaa bab' 'echo !!:gs/a/X/' 'echo !!:&' 'echo aaa' 'echo !!:g&'
run 'echo foo bar' '^foo^baz^' 'echo foo bar' '^bar^qux' '^nomatch^x^'
run 'echo hi' 'echo !nosuch' 'echo rc=$?' 'echo !1:9' 'echo !1:z' 'echo !!:&'
run 'echo hi!' 'echo ! x' 'x=1; [[ $x != 2 ]] && echo ne' 'sleep 0 & echo ${!}x' 'v=x; x=7; echo ${!v}' 'echo "a!"' 'echo a!=b' 'echo [!a]'
run 'echo a !#' 'echo b !#:1'
run "histchars='+=#'" 'echo hi' 'echo ++' 'echo !!' '=hi=yo'
run 'echo first' 'for i in 1; do' 'echo !!' 'done'
run 'echo first' 'cat <<EOF' 'body !!' 'EOF'
run 'echo first' "echo \"a'!!'b\"" 'set +H' 'echo !!'
run 'echo one' 'echo two' 'history -p "!!" "!-2"' 'fc -l'
run 'echo a b c' 'echo !!:1-$' 'echo !!:0-1'
run 'echo x' 'echo !!; echo !!'
run 'echo x' 'echo ${x:-!!}' 'echo $((1!=2))'
echo end
