c=''; case "" in "$c") echo m1;; esac; case "" in $c) echo m2;; esac; case x in $c) echo m2b;; esac
c=$'\r'; case "$c" in "$c") echo m3;; esac; case "$c" in $'\r') echo m4;; esac; d=$(printf '\r'); echo ${#d}
c=$(printf '\x0d'); case "$c" in "$c") echo m5;; esac; echo ${#c}
