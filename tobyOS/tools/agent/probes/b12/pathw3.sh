cd /tmp; B=~/bash-5.2.15/bash
$B -c 'PATH=/tmp/pw1:$PATH; noexec; echo st=$?' 2>&1
$B -c 'PATH=/tmp/pw1:$PATH; badint; echo st=$?' 2>&1
$B -c 'PATH=/tmp/pw1:$PATH; /tmp/pw1/badint; echo st=$?' 2>&1
$B -c 'cd /tmp/pw1; PATH=:/bin; badint; type badint; hash; echo st=$?' 2>&1
