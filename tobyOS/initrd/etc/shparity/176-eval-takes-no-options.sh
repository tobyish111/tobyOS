# eval takes no options (bash's no_options): `eval -- TEXT` drops the `--`,
# `eval -x` is "eval: -x: invalid option" and a usage line, status 2. tsh
# ran a command called `--` (127), so `eval -- "$(declare -p ...)"` -- how
# a $( ) hands variables back -- set nothing. The message prefix is cut off.
strip() { sed 's/^.*: line [0-9]*: //; s/^tsh: //' /tmp/ev176.$$; }
eval -- 'echo dashdash'; echo "status=$?"
eval --; echo "dashdash alone=$?"
eval -x 'echo x' 2>/tmp/ev176.$$; echo "-x status=$?"; strip
eval -nx 'echo x' 2>/tmp/ev176.$$; echo "-nx status=$?"; strip
x=$(eval -- 'echo in-cmdsub'); echo "[$x]"
eval -- "$(printf 'a\n' | { s=1; declare -p s; })"; echo "s=$s"
eval 'echo -- plain'
eval -- -- 2>/tmp/ev176.$$; echo "-- -- status=$?"; strip
rm -f /tmp/ev176.$$
echo done
