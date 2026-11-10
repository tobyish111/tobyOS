exec 2>&1 </dev/null
cd /tmp
rm -f /tmp/q35f /tmp/q35g /tmp/q35h
[[ -n x || $(echo ran1; touch /tmp/q35f) ]]; echo "rc=$? f=$([ -e /tmp/q35f ] && echo yes || echo no)"
[[ -z x && $(touch /tmp/q35g) == y ]]; echo "rc=$? g=$([ -e /tmp/q35g ] && echo yes || echo no)"
[[ a == a || $((q35x=5)) ]]; echo "q35x=${q35x-unset}"
[[ a == b && ${q35y:=7} ]]; echo "q35y=${q35y-unset}"
[[ -n $(echo hi >&2) ]]; echo "rc=$?"
set -- '$(touch /tmp/q35h)' '"' "'" 'a b'
for w; do printf '[%s]' "$w"; done; echo; echo "h=$([ -e /tmp/q35h ] && echo yes || echo no)"
rm -f /tmp/q35f /tmp/q35g /tmp/q35h
echo end
