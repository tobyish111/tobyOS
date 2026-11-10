hash whoami
echo status=$?
hash | grep -o /whoami  # prints it twice
hash _nonexistent_
echo status=$?

# mksh doesn't fail
