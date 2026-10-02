# A syntax error in each place a shell reads text, and what it ends: a
# script (ends it), eval (status 2, goes on), a sourced file (ends the
# file, status 2), `...` (ends the substitution), -c, an array literal
# (drops the line, $? 1), a [[ ]] error (no status of its own), set -e
# (exits after the first message), $( ) inside eval (ends everything).
# Each case runs in its own shell; stderr is folded in.
exec 2>&1
d=/tmp/synctx; mkdir -p $d; cd $d
SH=${SH:-bash}
run() { printf '%s\n' "$2" > $d/$1.sh; echo "=== $1"; $SH $1.sh; echo "exit=$?"; }
printf 'echo lib1\nfi\necho lib3\n' > lib.sh
run s1 $'echo before\nfi\necho after'
run s2 $'echo a\neval "echo in; fi"\necho "eval st=$?"\neval $\'echo x\\nfi\\necho y\'\necho "eval2 st=$?"\n. ./lib.sh\necho "source st=$?"\necho `fi`\necho "bq st=$?"\nf() { eval "fi"; echo "f st=$?"; }\nf\necho end'
run s3 $'echo a\n[[ a b ]]\necho after'
run s4 $'false\necho "x'
run s5 $'x=(1 & 2)\necho "after discard $?"\na=(\n\'2 3\'\n)\necho "two $?"'
run s6 $'echo $(fi)\necho after'
run s7 $'set -e\neval "fi"\necho after'
run s8 $'echo a\neval "echo \\$(fi)"\necho after'
run s9 $'false\n[[ a b ]]'
run s10 $'false\nx=(&)'
run s11 $'f() {\n  echo in f\n}\nf\nfi fi'
run s12 $'cat <<EOF\nbody'
run s13 $'echo a; eval "[[ a b ]]"; echo "st=$?"'
run s14 $'if true; then\n  echo x\nfi fi\necho no'
run s15 $'for i in 1 2; do\n  echo $i\ndone done'
run s16 $'echo a &&\n\necho b ||\n# c\necho c |\n\ncat'
run s17 $'case x in\n  x) echo x ;;\n  y) echo y ;;\nesac esac'
run s18 $'while :; do break; done | ( fi )'
run lk1 $'eval "[[ a b ]]"\necho "st=$?"\n[[ x ]] && echo ok1\necho end'
run lk2 $'eval "[[ a b ]]"\neval "[[ x ]]"; echo "st2=$?"\neval "[[ x ]]"; echo "st3=$?"\n[[ y ]] && echo ok\necho end'
run lk3 $'eval "[[ a b ]]"\neval "fi"\n[[ y ]] && echo ok\necho end'
run lk4 $'eval "[[ a b ]]"\neval "echo ]]"\necho end'
echo "=== -c"
$SH -c 'echo a; fi'; echo "exit=$?"
$SH -c $'echo a\nfi\necho b'; echo "exit=$?"
$SH -c 'fi' myname; echo "exit=$?"
echo done
