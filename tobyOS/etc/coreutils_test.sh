# Fixture + transcript for logs/coreutils.sh.
#
# It lives in the IMAGE so the ssh command that runs it is ~30 bytes. Sending
# the script itself failed two different ways: at 255 bytes the server
# silently truncated it (fixed in src/ssh.c), and at ~900 the client
# disconnects before the shell is even spawned. A file sidesteps both, and
# the SAME file is run by GNU bash on the host as the oracle.
cd "${WORKDIR:-/tmp}"
printf 'b\na\nc\n' > f1
printf '1 a\n2 b\n3 c\n' > s1
printf '1 x\n2 y\n3 z\n' > s2
printf '3 c\n1 a\n' > u1
printf 'one two three four five six seven eight nine ten\n' > words
printf 'hello\n' > h
echo @@sha224;  sha224sum h
echo @@sha384;  sha384sum h
echo @@b2sum;   b2sum h
echo @@b64;     basenc --base64 h
echo @@b32;     basenc --base32 h
echo @@b16;     basenc --base16 h
echo @@rt;      basenc --base64 h | basenc --base64 -d
echo @@tsort;   printf 'a b\nb c\n' | tsort
echo @@fmt;     fmt -w 20 words
echo @@join;    join s1 s2
echo @@joinbad; join u1 s2 2>&1; echo rc=$?
echo @@ptx;     ptx h
echo @@numto;   numfmt --to=si 1500
echo @@numtog;  numfmt --to=si 2500000000
echo @@numfrom; numfmt --from=si 1.5K
echo @@csplit;  csplit -f cs f1 2 > /dev/null; cat cs00; echo --; cat cs01
echo @@pr;      pr -t f1
echo @@pathbad; pathchk -p 'bad name!' 2>&1; echo rc=$?
echo @@pathok;  pathchk -p good_name; echo rc=$?

# --- tail/head window arithmetic -----------------------------------------
# `tail -n +2` used to mean `tail -n 2` here, because the count was read
# with atoi() and atoi("+2") is 2. Dropping a header line is the commonest
# use of tail in any script, and it was silently printing the LAST two lines
# instead. head had the mirror-image gap: a negative count was rejected as a
# usage error rather than meaning "all but the last N".
printf 'L1
L2
L3
L4
L5
' > five
printf 'abcdefghij' > ten
echo @@tailplus;   tail -n +2 five
echo @@tailplus1;  tail -n +1 five
echo @@tailplusbig; tail -n +9 five; echo rc=$?
echo @@taillast;   tail -n 2 five
echo @@tailneg;    tail -n -2 five
echo @@tailzero;   tail -n 0 five; echo rc=$?
echo @@tailshort;  tail -5 five
echo @@tailcplus;  printf "%s\n" "$(tail -c +4 ten)"
echo @@tailclast;  printf "%s\n" "$(tail -c 3 ten)"
echo @@tailmulti;  tail -n 1 five h
echo @@tailq;      tail -q -n 1 five h
echo @@headneg;    head -n -2 five
echo @@headneg1;   head -n -1 five
echo @@headnegall; head -n -9 five; echo rc=$?
echo @@headcneg;   printf "%s\n" "$(head -c -4 ten)"
echo @@headplain;  head -n 2 five
echo @@headq;      head -q -n 1 five h
echo @@END
