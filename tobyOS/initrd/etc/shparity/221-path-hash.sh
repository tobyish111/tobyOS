# The command hash table (hash, hash -p/-d/-t/-r, PATH changes reset it),
# a prefix PATH, and an empty or unset PATH -- tsh had no table.
# (batch 12; probe p4 in C:\t\b12)
exec 2>&1
cd /tmp; rm -rf pw; mkdir -p pw/a pw/b pw/c; printf 'echo A\n' > pw/a/zcmd; printf 'echo B\n' > pw/b/zcmd; chmod +x pw/a/zcmd pw/b/zcmd
printf 'echo noexec\n' > pw/c/zne; printf '#!/nonexist/interp\necho y\n' > pw/c/zbad; chmod +x pw/c/zbad
OPATH=$PATH
PATH=/tmp/pw/b:$OPATH; hash; zcmd; hash; zcmd; hash; type zcmd; command -v zcmd; hash -t zcmd; hash -l
PATH=/tmp/pw/a:/tmp/pw/b:$OPATH; hash; zcmd; hash
PATH=/tmp/pw/b:$OPATH; zcmd; PATH=/tmp/pw/a:/tmp/pw/b:$OPATH
PATH=/tmp/pw/a:$OPATH zcmd; hash
PATH=/tmp/pw/c:$OPATH; zne; echo st=$?; zbad; echo st=$?
hash -d; echo st=$?; hash -t; echo st=$?; hash -p /tmp nm; echo st=$?; hash -p /bin/ls 1; hash -p /bin/ls 'a b'; hash -l; hash -x; echo st=$?
hash -r; hash nosuchcmd; echo st=$?; hash cd; echo st=$?; hash ls; hash -t ls; hash
cd /tmp/pw/a; PATH=:/bin; zcmd; type zcmd; hash; cd /tmp
unset PATH; ./pw/a/zcmd; zzz; echo st=$?; PATH=$OPATH
