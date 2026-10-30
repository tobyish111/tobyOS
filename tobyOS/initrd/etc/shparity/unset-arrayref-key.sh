# unset of a word WRITTEN as an array reference -- `unset -v A[$k]` --
# takes the key as it is, with no second expansion and no quote in it a
# quote; a quoted word "A[$k]" is checked and expanded again as bash does
# (finding 369). tsh expanded every key again: ` was "bad substitution".
exec 2>&1
declare -A A; v=Z; for k in '$v' "8's" 'a]b' '`' '"' '\' '[a' Z; do A[$k]=1; done
for k in '$v' "8's" 'a]b' '`' '"' '\' '[a'; do unset -v A[$k]; done; declare -p A
declare -A B; for k in '$v' "8's" 'a]b' Z; do B[$k]=1; done
for k in '$v' "8's" 'a]b'; do unset -v "B[$k]"; echo "[$k] $?"; done; declare -p B
