# $( ) strips trailing NEWLINES only -- a CR stays; and a case pattern that
# expands to nothing matches the empty word. tsh stripped CRs too and
# skipped an empty pattern, so once printf made real bytes, Oils 1366
# (every byte matches itself) lost bytes 10 and 13.
exec 2>&1
c=''; case "" in "$c") echo m1;; esac; case "" in $c) echo m2;; esac; case x in $c) echo m2b;; esac
c=$'\r'; case "$c" in "$c") echo m3;; esac; case "$c" in $'\r') echo m4;; esac; d=$(printf '\r'); echo ${#d}
c=$(printf '\x0d'); case "$c" in "$c") echo m5;; esac; echo ${#c}
sum=0; for i in $(seq 1 255); do h=$(printf %x $i); c="$(printf "\x$h")"; case "$c" in "$c") sum=$((sum+1));; *) echo "miss $i";; esac; done; echo sum=$sum
