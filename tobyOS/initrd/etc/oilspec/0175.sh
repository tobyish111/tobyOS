case $SH in zsh|ash) exit ;; esac

# This tests that the worse parser doesn't unconditinoally treat a[ as special

a[1 + 2]= argv.py a[1 + 2]=
echo status=$?

a[1 + 2]+= argv.py a[1 + 2]+=
echo status=$?

argv.py a[3 + 4]=

argv.py a[3 + 4]+=
