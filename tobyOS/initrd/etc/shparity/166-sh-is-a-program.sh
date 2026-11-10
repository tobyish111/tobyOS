# `sh` is /bin/sh, found through PATH -- not a builtin (S8). /bin/tsh ran it
# as a builtin in the CALLER's process: `sh -c 'echo $$'` printed the
# caller's pid, `type sh` said "a shell builtin", a script run as `sh script`
# shared the caller's traps and variables, and `sh -c 'kill -9 $$'` killed
# the caller. `enable` listed and accepted it too (with the kernel console's
# cat and ls).
p=$$; c=$(sh -c 'echo $$')
if [ "$c" != "$p" ]; then echo "sh -c has its own pid"; else echo "sh -c shares the caller's pid"; fi
type sh
command -V sh
enable sh 2>/dev/null; echo "enable sh: $?"
echo "enable lists sh/cat/ls: $(enable | grep -c -x -e 'enable sh' -e 'enable cat' -e 'enable ls')"
printf 'trap "echo trap-in-child" EXIT\nv=set-in-child\n' > /tmp/fx166.sh
sh /tmp/fx166.sh
echo "caller v=[${v-unset}]"
trap - EXIT
# bash reports the KILL with the child's pid on stderr; keep it out.
{ sh -c 'kill -9 $$'; } 2>/dev/null
echo "survived: $?"
rm -f /tmp/fx166.sh
echo done
