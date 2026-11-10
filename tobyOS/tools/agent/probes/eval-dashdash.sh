eval -- 'echo plain-eval-dashdash'
x=$(printf 'a\n' | { s=1; declare -p s; }); echo "pipe-in-cmdsub=[$x]"
x=$(s=2; declare -p s); echo "declare-in-cmdsub=[$x]"
x=$( { s=3; declare -p s; } ); echo "group-in-cmdsub=[$x]"
eval -- "$(echo 'echo via-cmdsub')"
