# What $? does the RETURN trap see after `.`, and what is $? after it?
trap 'echo "  trap sees $?"' RETURN
echo "false; . missing"; false; . ./nosuch_drs.sh 2>/dev/null; echo "  after=$?"
echo "true; . missing"; true; . ./nosuch_drs.sh 2>/dev/null; echo "  after=$?"
echo "false; . dir"; false; . /tmp 2>/dev/null; echo "  after=$?"
printf 'return 3\n' > /tmp/drs3.sh
echo "false; . return 3"; false; . /tmp/drs3.sh; echo "  after=$?"
printf 'false\n' > /tmp/drs4.sh
echo "true; . false"; true; . /tmp/drs4.sh; echo "  after=$?"
printf 'true\n' > /tmp/drs5.sh
echo "false; . true"; false; . /tmp/drs5.sh; echo "  after=$?"
trap - RETURN
rm -f /tmp/drs3.sh /tmp/drs4.sh /tmp/drs5.sh
echo done
