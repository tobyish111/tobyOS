cd /tmp; mkdir -p /tmp/pw1 /tmp/pw2/dd; printf 'echo x\n' > /tmp/pw1/noexec; chmod -x /tmp/pw1/noexec
printf '#!/nonexist/interp\necho y\n' > /tmp/pw1/badint; chmod +x /tmp/pw1/badint
B=~/bash-5.2.15/bash
$B -c 'PATH=/tmp/pw1:$PATH; noexec; echo st=$?'
$B -c 'PATH=/tmp/pw1:$PATH; badint; echo st=$?'
$B -c 'PATH=/tmp/pw1:$PATH; hash; type badint; noexec 2>/dev/null; hash; hash -l; hash -t badint noexec; hash -t badint'
$B -c 'hash -d; echo st=$?; hash -t; echo st=$?; hash -p /tmp nm; echo st=$?; hash -p /bin/ls 1; hash -p /bin/ls "a b"; hash -l; hash -x; echo st=$?'
