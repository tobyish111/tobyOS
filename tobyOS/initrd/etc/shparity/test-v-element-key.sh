# [[ -v A[$k] ]] asks about the key as the word was expanded, once; test -v
# "A[$k]" checks the word as bash does and expands its key again (finding
# 370). tsh expanded [[ ]]'s key again and took test's `A[a]b]`.
exec 2>&1
declare -A A; v=Z; for k in '$v' "8's" 'a]b' '\' '[a'; do A[$k]=1; done
for k in '$v' "8's" 'a]b' '\' '[a' Z; do [[ -v A[$k] ]]; echo -n "[$k] $? "; test -v "A[$k]"; echo "$?"; done
