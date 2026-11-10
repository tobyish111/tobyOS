# Account-tool transcript for logs/accounts.sh.
#
# In the image, run by the boot harness -- no SSH. See logs/coreutils.sh for
# why: sessions wedge under process churn and turn one hung session into a
# whole run of phantom failures.
#
# These are NOT compared against GNU: shadow-utils is not installed on the
# host, and the interesting behaviour is what lands in /etc/passwd and
# /etc/group, which is checkable directly.
# Clean up first. The mirror to /data/etc really does survive a reboot --
# which is the point of it -- so a second run would otherwise find the users
# this script created last time and every 'already exists' would look like a
# bug. Errors here are expected on a first run and are discarded.
userdel alice  > /dev/null 2>&1
userdel bob    > /dev/null 2>&1
groupdel testers > /dev/null 2>&1
groupdel qa      > /dev/null 2>&1
echo @@start
echo -- passwd before --; cat /etc/passwd
echo @@useradd
useradd -u 1500 -g 100 -c "Test User" -s /bin/tsh alice 2>&1
grep alice /etc/passwd
echo @@dupe
useradd alice 2>&1; echo rc=$?
echo @@autouid
useradd bob 2>&1 | grep -v NOTE
grep bob /etc/passwd
echo @@groupadd
groupadd -g 400 testers
grep testers /etc/group
echo @@gpasswd_add
gpasswd -a alice testers
grep testers /etc/group
echo @@gpasswd_twice
gpasswd -a alice testers
grep testers /etc/group
echo @@gpasswd_del
gpasswd -d alice testers
grep testers /etc/group
echo @@usermod_shell
usermod -s /bin/sh alice
grep alice /etc/passwd
echo @@usermod_G
usermod -G testers alice
grep testers /etc/group
echo @@chsh_bad
chsh -s /bin/nosuchshell alice 2>&1; echo rc=$?
echo @@chsh_good
chsh -s /bin/tsh alice; grep alice /etc/passwd
echo @@chfn
chfn -f "Alice A" alice; grep alice /etc/passwd
echo @@groupmod
groupmod -n qa testers; grep qa /etc/group
echo @@chage
chage -l alice | head -2
echo @@userdel
userdel alice; grep -c alice /etc/passwd
echo @@userdel_purges_group
grep qa /etc/group
echo @@groupdel
groupdel qa; grep -c qa /etc/group
echo @@missing
userdel nosuchuser 2>&1; echo rc=$?
echo @@mirror
cat /data/etc/passwd | grep -c bob
echo @@END
