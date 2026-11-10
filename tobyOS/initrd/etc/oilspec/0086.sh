a=(a b c d e f)

# space required here -- see spec/var-op-slice
echo slice ${a[@]: }
echo status=$?
echo

echo slice ${a[@]: : }
echo status=$?
echo

# zsh doesn't accept this
echo slice ${a[@]:: }
echo status=$?
