# A command that cannot be found, in bash's words (S10): "NAME: command not
# found", "PATH: No such file or directory", "exec: NAME: not found", 127.
# tsh said "spawn: failed to launch '/bin/NAME'" for every one of them. The
# prefix in front of the message ("SCRIPT: line N: ") is S12's, cut off here.
p() { sed -e 's/^[^:]*: line [0-9]*: //' -e 's/^tsh: //'; }
{ nosuchcmd_fx170; echo "rc=$?"; } 2>&1 | p
{ ./nosuch_fx170; echo "rc=$?"; } 2>&1 | p
{ /tmp/nosuch_fx170/abs; echo "rc=$?"; } 2>&1 | p
{ nosuchcmd_fx170 | cat; echo "rc=$?"; } 2>&1 | p
{ echo x | ./nosuch_fx170; echo "rc=$?"; } 2>&1 | p
{ ( exec nosuchcmd_fx170 ); echo "rc=$?"; } 2>&1 | p
{ command nosuchcmd_fx170; echo "rc=$?"; } 2>&1 | p
{ x=1 nosuchcmd_fx170; echo "rc=$?"; } 2>&1 | p
f() { nosuch_in_fn_fx170; }; { f; echo "rc=$?"; } 2>&1 | p
nosuchcmd_fx170 2>/dev/null; echo "quiet rc=$?"
echo done
