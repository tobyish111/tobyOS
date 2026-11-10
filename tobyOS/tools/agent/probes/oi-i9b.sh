cd /tmp; for i in $(seq 40); do pushd /tmp >/dev/null 2>&1; done; echo "dirs $(dirs -p 2>/dev/null | wc -l)"
n=0; for c in $(ls /bin | head -80); do hash -p /bin/$c $c 2>/dev/null && n=$((n+1)); done; echo "hashed-ok $n listed $(hash 2>/dev/null | tail -n +2 | wc -l)"
for i in $(seq 40); do complete -W x cmd$i 2>/dev/null; done; echo "complete $(complete -p 2>/dev/null | grep -c 'cmd[0-9]')"
