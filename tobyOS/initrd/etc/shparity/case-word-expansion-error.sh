# A case word that fails to expand is that expansion's error, as in any
# other word: it has said why, the rest of the line goes and the next line
# sees 1 -- or the shell ends, for `${x:?}` or under set -e. tsh said
# "case: word too long" as well, left status 2 and ran on (finding 622).
exec 2>&1
case ${a[} in *) echo arm;; esac; echo "same line"
echo "next line: $?"
case ${!nosuch} in *) echo arm;; esac; echo "same line"
echo "next line: $?"
f() { case ${a[} in *) ;; esac; echo "in f"; }; f; echo "after f"
echo "after the function: $?"
eval 'case ${a[} in *) ;; esac'; echo "after eval: $?"
( case ${zz:?gone} in *) ;; esac; echo "not in the subshell" ); echo "subshell: $?"
( set -e; case ${a[} in *) ;; esac; echo "not under set -e" ); echo "set -e: $?"
