exec 2>&1
for s in PROF WINCH CHLD URG; do
  "$BASH" -c 'trap "echo exit-'$s'" EXIT; kill -'$s' $$; echo after-'$s; echo "h10 $s st=$?"
done
echo end
