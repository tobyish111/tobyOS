# A process substitution's descriptor is the highest number from 63 down
# that nothing holds -- a 63 the script opened too -- and with 63 at or
# past the soft open-file limit, the pipe's own, the lowest free. tsh
# counted down from 63 over whatever held the number, and under a limit of
# 63 or less said "no descriptor 63" (finding 650).
exec 2>&1
echo <(true) <(true) >(true)
exec 63>/dev/null; echo "63 taken: " <(true); cat <(echo ok); exec 63>&-
(ulimit -n 64; echo "limit 64: " <(true))
(ulimit -n 63; echo "limit 63: " <(true); cat <(echo ok63))
(ulimit -n 50; echo "limit 50: " <(true) <(true))
(ulimit -n 10; cat <(echo ok10))
