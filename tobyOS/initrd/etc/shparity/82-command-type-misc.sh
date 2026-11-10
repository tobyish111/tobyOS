# XCU 4 introspection corners: command -v/-V, type, times, set -h, hash -r.
command -v echo
echo "1=$?"
command -v nosuchcmd
echo "2=$?"
command -v /bin/does-not-exist
echo "3=$?"
type echo
command -V echo
times > /dev/null
echo "4=$?"
set -h; echo "5=$?"
set +h; echo "6=$?"
hash -r; echo "7=$?"
echo end
