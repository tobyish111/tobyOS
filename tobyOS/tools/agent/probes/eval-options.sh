# eval takes no options: `--` is dropped, `-x` is a usage error (bash's
# no_options). The message prefix is cut off (it is S12's business).
strip() { sed 's/^.*: line [0-9]*: //; s/^tsh: //' /tmp/ev.$$; }
eval -- 'echo dashdash'; echo "status=$?"
eval --; echo "dashdash alone=$?"
eval -x 'echo x' 2>/tmp/ev.$$; echo "-x status=$?"; strip
eval -nx 'echo x' 2>/tmp/ev.$$; echo "-nx status=$?"; strip
x=$(eval -- 'echo in-cmdsub'); echo "[$x]"
eval -- "$(printf 'a\n' | { s=1; declare -p s; })"; echo "s=$s"
eval 'echo -- plain'
eval -- -- 2>/tmp/ev.$$; echo "-- -- status=$?"; strip
rm -f /tmp/ev.$$
echo done
