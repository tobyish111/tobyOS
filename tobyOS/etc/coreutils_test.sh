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
echo @@END
