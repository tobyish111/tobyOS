# failglob's "no match" names the pattern with its quoting gone: `x\*[ab]`
# is "no match: x*[ab]". tsh's message carried the escape bytes it marks a
# quoted character with (finding 577).
exec 2>&1
cd "$(mktemp -d)" || exit 1
shopt -s failglob
echo x\*[ab]; echo "rc=$?"
echo "x*"[ab]
echo 'y?'[ab] z
