# ulimit's table, its -a layout and units; tsh printed its own format.
# (batch 12; probe p7 in C:\t\b12)
exec 2>&1
ulimit; ulimit -a; ulimit -Ha | head -3; ulimit -Sn; ulimit -Hn; ulimit -s; ulimit -c -f -n; ulimit -p; ulimit -p 8; echo st=$?
ulimit -t 100; echo st=$?; ulimit -t; ulimit -f abc; echo st=$?; ulimit -Z; echo st=$?; ulimit -f 99999999999999999999999; echo st=$?
ulimit -a 42; echo st=$?; ulimit --all; echo st=$?; ulimit -f unlimited; echo st=$?; ulimit -H; ulimit 5; echo st=$?
