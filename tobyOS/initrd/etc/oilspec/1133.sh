IFS='\ '
echo "hello\ world  test" | (read a b; argv.py "$a" "$b")
IFS='\'
echo "hello\ world  test" | (read a b; argv.py "$a" "$b")
# In mksh/zsh, IFS='\' is stronger than backslash escaping
