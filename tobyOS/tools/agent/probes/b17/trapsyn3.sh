# trapsyn3.sh: the line a RETURN or ERR trap's action counts on from, in a
# function and a sourced file -- $LINENO in a good action next to a syntax
# error in a bad one (finding 36). The shell's own name and the script's
# path are taken out.
exec 2>&1
f=${TMP:-/tmp}/trapsyn3.$$.sh
g=${TMP:-/tmp}/trapsyn3s.$$.sh
r() { $SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s#$g#SOURCED#; s/^[a-z]*: /SH: /"; echo "rc=${PIPESTATUS[0]}"; }
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  :\n  :\n}\n\nf\necho after\n' > "$f"; r
printf 'f() {\n  trap '"'"'fi'"'"' RETURN\n  :\n  :\n}\n\nf\necho after\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R1=$LINENO\necho R2=$LINENO'"'"' RETURN\n  :\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo r1\nfi'"'"' RETURN\n  :\n  :\n}\n\nf\n' > "$f"; r
printf 'f() { trap '"'"'echo R=$LINENO'"'"' RETURN; :; }\n\nf\n' > "$f"; r
printf 'f() {\n  :\n  return 3\n  :\n}\ntrap '"'"'echo R=$LINENO'"'"' RETURN\nset -T\n\nf\necho "st=$?"\n' > "$f"; r
printf 'f() {\n  :\n  return 3\n  :\n}\ntrap '"'"'fi'"'"' RETURN\nset -T\n\nf\necho "st=$?"\n' > "$f"; r
printf ':\n:\necho in-sourced\n' > "$g"
printf 'trap '"'"'echo R=$LINENO'"'"' RETURN\n\n. %s\necho after\n' "$g" > "$f"; r
printf 'trap '"'"'fi'"'"' RETURN\n\n. %s\necho after\n' "$g" > "$f"; r
printf 'f() {\n  :\n  false\n  :\n}\nset -E\ntrap '"'"'echo E=$LINENO'"'"' ERR\n\nf\n' > "$f"; r
printf 'f() {\n  :\n  false\n  :\n}\nset -E\ntrap '"'"'fi'"'"' ERR\n\nf\n' > "$f"; r
printf '\n\ntrap '"'"'echo e=$LINENO'"'"' EXIT\n\nf() { exit 3; }\n\nf\n' > "$f"; r
printf 'trap '"'"'echo s=$LINENO'"'"' USR1\n\n\nkill -USR1 $$\n' > "$f"; r
printf '\nf()\n{\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  for i in 1; do\n    :\n  done\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  case x in\n    x) : ;;\n  esac\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  if true; then\n    :\n  fi\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  if true; then\n    return 2\n  fi\n}\n\nf\n' > "$f"; r
printf '\n\ntrap '"'"'nosuch_cmd_q'"'"' EXIT\n\necho x\n' > "$f"; r
printf 'trap '"'"'echo s=$LINENO; nosuch_cmd_s'"'"' USR1\n\n\nkill -USR1 $$\n' > "$f"; r
rm -f "$f" "$g"
echo end
