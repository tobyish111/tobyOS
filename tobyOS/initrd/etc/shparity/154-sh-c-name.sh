sh -c 'echo "1 $# [$0]"' _
sh -c 'echo "2 $# [$0] [$1] [$2]"' name a b
sh -c 'echo "3 $# [$0]"'
sh -c 'echo "4 [$*]"' x 'one two' three
set -- p q; sh -c 'echo "5 $#"' me "$@"
printf '%s\n' f1 f2 | while read -r f; do sh -c 'echo "6 [$1]"' _ "$f"; done
sh -c 'shift; echo "7 $# [$1]"' _ a b
echo done
