# `help` alone: bash's paragraph and its two columns of synopses, half the
# width each, cut with a `>` -- differently in a multibyte locale (bash's
# dispcolumn and wdispcolumn). The first line names the MACHTYPE, which is
# tsh's own by decision, so it is dropped. A disabled builtin gets a `*`.
skip1() { IFS= read -r l; while IFS= read -r l; do printf '%s\n' "$l"; done; }
for c in 80 81 100 120 60 40 30 9 8 7 6 3 1 0 -5 abc ' 70' 70x 255 256 300 ''; do
  for loc in C POSIX en_US.UTF-8 ''; do
    echo "== COLUMNS=[$c] LC_ALL=[$loc]"
    ( LC_ALL=$loc; COLUMNS=$c; help | skip1 )
    echo "rc=$?"
  done
done
echo "== LANG and LC_CTYPE"
( unset LC_ALL; LANG=C; COLUMNS=100; help | skip1 )
( unset LC_ALL; LC_CTYPE=C LANG=en_US.UTF-8; COLUMNS=100; help | skip1 )
( LC_ALL=en_US.UTF-8; LC_CTYPE=C; COLUMNS=60; help | skip1 )
echo "== disabled"
( enable -n echo printf test; COLUMNS=80; LC_ALL=C; help | skip1 )
( enable -n echo printf test; COLUMNS=100; LC_ALL=C.UTF-8; help | skip1 )
echo "== COLUMNS unset, not a terminal"
( unset COLUMNS; LC_ALL=C; help | skip1; echo "COLUMNS=${COLUMNS-unset} LINES=${LINES-unset}" )
