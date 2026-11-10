# Under fc -e, a line of the edited file that does not parse is echoed and
# recorded, as the lines before it are, and then its error stops the file
# -- tsh stopped before it (finding 597).
f=$(mktemp)
cat > "$f" <<'EOF'
unset HISTFILE
set -o history
ed4() { printf 'echo one\nif then\necho two\n' > "$1"; }
echo a
fc -e ed4 -1
echo "rc=$?"
fc -l
EOF
"$BASH" "$f" 2>&1 | sed 's/^[^ ]*-fc\.[^:]*: line /fcfile: line /'
rm -f "$f"
