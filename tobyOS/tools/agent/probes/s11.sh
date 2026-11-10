# S11: the shell run on a script it cannot read. $me is the shell running
# this file (/bin/bash or /bin/tsh), found through /proc.
me=$(readlink /proc/$$/exe)
echo "me=${me##*/}"
"$me" /tmp/nope_s11.sh; echo "missing rc=$?"
mkdir -p /tmp/s11d; "$me" /tmp/s11d; echo "dir rc=$?"; rmdir /tmp/s11d
"$me" /bin/ls; echo "binary rc=$?"
printf 'echo ran\n' > /tmp/s11r.sh; chmod 000 /tmp/s11r.sh
"$me" /tmp/s11r.sh; echo "unreadable(root) rc=$?"; rm -f /tmp/s11r.sh
cd /tmp; "$me" nope_rel.sh; echo "relative rc=$?"
"${me##*/}" /tmp/nope_s11.sh; echo "by name rc=$?"
echo done
