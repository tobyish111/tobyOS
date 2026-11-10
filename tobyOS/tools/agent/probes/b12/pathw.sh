cd /tmp; printf '#!/bin/sh\necho ran-dot\n' > /tmp/zzcmd; chmod +x /tmp/zzcmd
B=~/bash-5.2.15/bash
env -i $B -c 'echo "[$PATH]"'
$B -c 'unset PATH; zzcmd; echo st=$?'
$B -c 'PATH=; zzcmd; echo st=$?'
$B -c 'PATH=/nonexist:; zzcmd; echo st=$?'
$B -c 'PATH=/nonexist::/x; zzcmd; echo st=$?'
$B -c 'PATH=/nonexist; zzcmd; echo st=$?'
cd /
$B -c 'unset PATH; zzcmd; echo st=$?'
