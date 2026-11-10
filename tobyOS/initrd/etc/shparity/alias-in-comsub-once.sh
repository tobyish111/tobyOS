# A command substitution's aliases are taken once: when it runs, outside
# posix mode, so a chain is not taken twice (`echo $(say c)` with
# say='echo S' and echo='echo E' is "E S c", where tsh said "E E S c"),
# and over lines too; in posix mode as the line is read, wherever the $( )
# is -- in double quotes, in an assignment's value -- so an alias can make
# `case` of it, open a paren in it, or close it. And an alias that opens a
# group or a loop has the lines after it read with their aliases (finding
# 592).
f=$(mktemp)
cat > "$f" <<'EOF'
shopt -s expand_aliases
alias say='echo S'
alias echo='echo E'
\echo "[$(say a)]" $(say c)
v=$(say b); \echo "$v"
unalias echo
alias DO='{ '
alias DONE='}'
got=$(DO
echo nine; DONE)
echo "$got"
DO
echo group; DONE
alias LOOP='for i in 1 2; do' END='done'
LOOP
echo "loop $i"
END
f() { echo "$(say d)"; }
alias say='echo T'; f
set -o posix
alias switch=case
alias nest='('
alias short='echo eight )'
echo "$( switch foo in foo) echo three;; esac )"
echo "$( nest echo seven ) )"
x=$( switch b in (b) echo ex;; esac ); echo "$x"
echo $( short
echo "$( short "
alias say='echo S'
g() { echo "$(say g)"; }
alias say='echo T'; g
EOF
"$BASH" "$f" 2>&1
rm -f "$f"
