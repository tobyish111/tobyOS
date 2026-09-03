# SSH CLIENT transcript for logs/sshclient.sh.
#
# The client connects to tobyOS's OWN sshd over loopback. That is the
# strongest test available without a second machine: it drives the complete
# handshake -- version exchange, KEXINIT, curve25519, the Ed25519 host-key
# signature, key derivation, chacha20-poly1305 framing, userauth, and a
# channel -- against a server that is known-good because real OpenSSH talks
# to it every day.
echo @@keygen
ssh-keygen -f /data/ssh/test_id 2>&1 | head -3
echo @@keygen_files
test -f /data/ssh/test_id && echo private-ok
test -f /data/ssh/test_id.pub && echo public-ok
head -c 11 /data/ssh/test_id.pub; echo
echo @@keygen_refuses_passphrase
ssh-keygen -f /data/ssh/x -N secret 2>&1; echo rc=$?
echo @@keygen_refuses_rsa
ssh-keygen -t rsa 2>&1; echo rc=$?
echo @@exec
ssh -o StrictHostKeyChecking=no toby@127.0.0.1 'echo hello-from-remote' 2>&1
echo @@exec_status
ssh toby@127.0.0.1 'exit 7' 2>&1; echo rc=$?
echo @@pipeline
ssh toby@127.0.0.1 'echo a b c | wc -w' 2>&1
echo @@exec_long
# 3200 bytes in ONE argv element. The client spawn, the server's exec
# handler and `tsh -c` all capped an element at 512 and cut it SILENTLY:
# 500 bytes worked, 660 printed nothing. The whole command must run.
L="$(i=0; while [ $i -lt 400 ]; do i=$((i+1)); printf 'echo x; '; done)echo END"
echo "len=${#L}"
ssh toby@127.0.0.1 "$L" 2>&1 | tail -1
echo @@knownhosts
grep -c '127.0.0.1 ssh-ed25519' /data/ssh/known_hosts
echo @@badport
ssh -p 59999 toby@127.0.0.1 'echo nope' 2>&1; echo rc=$?
echo @@sshadd
ssh-add 2>&1; echo rc=$?
echo @@sftp
sftp toby@127.0.0.1 2>&1; echo rc=$?
echo @@END
