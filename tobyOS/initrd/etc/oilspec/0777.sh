# bash adds ]] and } and coproc

# Use bash as an oracle
bash -c 'compgen -k' | sort > bash.txt

# osh vs. bash, or bash vs. bash
$SH -c 'compgen -k' | sort > this-shell.txt

#comm bash.txt this-shell.txt

# show lines in both files
comm -12 bash.txt this-shell.txt | egrep -v 'coproc|select'
