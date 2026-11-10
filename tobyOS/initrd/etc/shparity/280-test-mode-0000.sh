# A FILE CHMOD'ED 000 HAS NO PERMISSION BITS, and `test` says so (batch
# 14): the mode stat(2) reports is the file's own -- the kernel answers for
# a filesystem without modes itself -- so /bin/tsh must not read a 0000 as
# "no permission information". It did once the kernel stopped reporting a
# 0000 as 0644: `test -x` said yes. Root still reads and writes a 0000
# file, and still needs an execute bit to execute one; so do bash's tests.
cd /tmp
rm -f fx280a fx280b
echo x > fx280a; echo x > fx280b
chmod 000 fx280a; chmod 100 fx280b
for f in fx280a fx280b; do
    r=n; w=n; x=n
    [ -r $f ] && r=y; [ -w $f ] && w=y; [ -x $f ] && x=y
    echo "$f r=$r w=$w x=$x"
done
[[ -x fx280a ]] && echo "[[ -x ]] yes" || echo "[[ -x ]] no"
chmod 644 fx280a fx280b
rm -f fx280a fx280b
echo end
