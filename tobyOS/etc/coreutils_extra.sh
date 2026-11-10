# Guest-only cases for logs/coreutils.sh: no GNU oracle for these.
# In the image for the same reason as coreutils_test.sh -- a ~340-byte exec
# command wedged the session often enough to lose whole runs.
cd /tmp
echo @@dir;       dir f1
echo @@vdir;      vdir f1 | grep -c f1
echo @@dircolors; dircolors | grep -c LS_COLORS
echo @@dircsh;    dircolors -c | grep -c setenv
echo @@pinky;     pinky | grep -c root
echo @@chcon;     chcon -t x h 2>&1; echo rc=$?
echo @@runcon;    runcon x /bin/true 2>&1; echo rc=$?
echo @@stdbuf;    stdbuf -o0 echo ran 2>/dev/null
echo @@END
