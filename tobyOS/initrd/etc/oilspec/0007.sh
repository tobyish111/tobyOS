alias e=echo ll='ls -l'
unalias e nonexistentZ ll
echo status=$?
