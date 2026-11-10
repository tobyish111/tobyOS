# The shell started on a script it cannot run (S11): bash exits 127 for a
# missing file and 126 for a directory or a binary, with its own name -- or,
# once $0 is the script, the script's -- in front of the reason. tsh exited
# 1, and ran a binary's bytes as shell text. $me is the shell running this
# file; its name is replaced by SHELL so the two runs can be compared.
me=$(readlink /proc/$$/exe)
"$me" /tmp/nope_fx171.sh 2>/dev/null; echo "missing: $?"
"$me" /tmp/nope_fx171.sh 2>&1 | sed "s#^$me: #SHELL: #"
mkdir -p /tmp/fx171d; "$me" /tmp/fx171d 2>&1; echo "directory: $?"; rmdir /tmp/fx171d
"$me" /bin/ls 2>&1; echo "binary: $?"
printf '#!/bin/sh\necho text ran\n' > /tmp/fx171t.sh; "$me" /tmp/fx171t.sh; echo "text: $?"; rm -f /tmp/fx171t.sh
cd /tmp; "$me" nope_rel_fx171.sh 2>&1 | sed "s#^$me: #SHELL: #"
echo done
