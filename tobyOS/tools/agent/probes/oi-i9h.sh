HISTSIZE=1000; set -o history; history -c; for i in $(seq 300); do history -s "cmd $i"; done 2>/dev/null; echo "history $(history | wc -l)"
echo "histsize-var [$HISTSIZE]"
