/ 2>/dev/null; echo "G1 st=$?"
cd / && ./tmp 2>/dev/null; echo "G2 st=$?"
x=$(/ 2>&1); case "$x" in *"Is a directory"*) echo "G3 is-a-directory";; *) echo "G3 other";; esac
/nope_g/cmd 2>/dev/null; echo "G4 st=$?"
echo done
