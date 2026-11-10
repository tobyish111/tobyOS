# bash expands a simple command's words first -- the command word and its
# arguments, in order -- then its assignments, then each redirection as it
# applies it, wherever each was written; so a redirection written before the
# command word is still the command's, and an external command's error
# there is its failure, status 1, the line going on. tsh expanded left to
# right, and that redirection's error discarded the line (finding 641,
# closing 629's STILL OPEN part).
exec 2>&1
< $(echo R >&2; echo /dev/null) cat $(echo W >&2; echo /dev/null)
> $(echo R >&2; echo /dev/null) echo $(echo W >&2; echo w)
x=$(echo X >&2) cat $(echo W >&2; echo /dev/null) < $(echo R >&2; echo /dev/null)
x=$(echo X >&2) > $(echo R >&2; echo /dev/null) y=$(echo Y >&2)
f() { :; }; > $(echo R >&2; echo /dev/null) f $(echo W >&2)
x=$(exit 3) echo "the word saw $?"
x=$v sh -c 'echo "x=$x"' ${v:=wordset}
i=0; x=$((i++)) sh -c 'echo "x=$x"' $((i++)); echo "i=$i"
<<< ${a[} cat; echo "cat's here-string, written first: $?"
< ./in$((1+)) cat; echo "cat's input, written first: $?"
<<< ${a[} cat $(echo W >&2); echo "after the word's comsub: $?"
cat <<< ${a[} ${b[}; echo "not after the word's error"
echo "the word's error came first: $?"
echo x | > $(echo R >&2; echo /dev/null) cat $(echo W >&2; echo /dev/null)
eval '< $(echo R >&2; echo /dev/null) cat $(echo W >&2; echo /dev/null)'
< $(echo R >&2; echo /dev/null) cat $(echo W >&2; echo /dev/null) & wait
