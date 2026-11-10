# A case word, [[ ]]'s words and a here-string are strings: under a null
# IFS their $* and ${a[*]} run the elements together -- the word of a
# ${A-W} in them joins with a space -- and with IFS set a case word's
# ${a[@]#p} joins with a space. tsh joined everything with spaces
# (finding 560).
exec 2>&1
set -- a 'b c' d; arr=(a 'b c' d)
IFS=
case $* in 'ab cd') echo c1;; *) echo c1-no;; esac
case ${arr[*]} in 'ab cd') echo c2;; *) echo c2-no;; esac
case ${A-$*} in 'a b c d') echo c3;; *) echo c3-no;; esac
case ${A-${arr[*]}} in 'a b c d') echo c4;; *) echo c4-no;; esac
[[ $* == 'ab cd' ]] && echo d1
[[ ${A-${arr[*]}} == 'a b c d' ]] && echo d2
cat <<< $*
cat <<< ${A-$*}
cat <<< ${arr[*]}
IFS=-
case ${arr[@]#x} in 'a b c d') echo c5;; *) echo c5-no;; esac
[[ ${arr[@]#x} == 'a b c d' ]] && echo d3
