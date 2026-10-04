exec 2>&1
f() { local -x a1=1; declare -p a1; env | grep '^a1='; }; f
x=g; export x; g() { local x=l; declare -p x; env | grep '^x='; }; g
env | grep '^_='; echo
env | tail -1; echo
cd /bin && ./env | grep -c . ; cd /tmp
cd /bin && ./true; echo "rc=$?"; cd /tmp
cd /bin && ./echo hi; cd /tmp
echo end
