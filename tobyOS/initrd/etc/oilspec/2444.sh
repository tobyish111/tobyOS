for num_bytes in 0 1 2 3 4 5 6 7 8 9 10 11 12 13; do
  s=$(head -c $num_bytes $REPO_ROOT/spec/testdata/utf8-chars.txt)
  echo ${#s}
done 2> $TMP/err.txt

grep 'warning:' $TMP/err.txt
true  # exit 0

# zsh behavior actually matches bash!
