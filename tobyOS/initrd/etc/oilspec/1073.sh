# OSH bug
# https://oilshell.zulipchat.com/#narrow/channel/502349-osh/topic/alpine.20build.20failures.20-.20make.20-.20ulimit.20-n.2064/with/519691301

$SH -c 'ulimit -n 64; echo hi >out'
echo status=$?

$SH -c 'ulimit -n 0; echo hi >out'
echo status=$?
