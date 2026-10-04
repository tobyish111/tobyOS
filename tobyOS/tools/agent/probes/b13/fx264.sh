d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# HISTORY EXPANSION (batch 13), on in a script that asks for it: each line
# is expanded as it is read, echoed, and is what runs and what history
# records; a failed one is reported and dropped -- and not counted, so the
# lines after it are numbered one less; :p records without running. Event
# designators, word designators, modifiers, ^old^new, !#, histchars, the
# multi-line rule, here-documents left alone, and history -p, which
# expands set -H or not. tsh had none of it. (logs/b13 q29 q38)
exec 2>&1
set -o history -H
echo one two three /usr/lib/x.tar.gz
echo !!:1 !$ !^ !*
echo !1:2-3 !1:1- !1:-2 !1:0
echo !1:5:h !1:5:t !1:5:r !1:5:e !1:5:t:r
echo aaa bab
echo !!:s/a/X/ next
echo !-2:gs/a/Y/
^bab^BAB
echo !?bab? !%
echo a "!!" '!!' \!! x!!y
echo !nosuch
echo "lineno=$LINENO"
echo !1:9
echo !!:z
echo [!a] ${!} $((1!=2)) a!=b hi!
echo a !# b
echo !!:p
for i in 1; do
echo !! inside
done
cat <<EOF
body !!
EOF
history -p '!!' '!-3:0' 'plain'
histchars='+=#'
echo ++
set +H
echo !!
history 5
echo end
