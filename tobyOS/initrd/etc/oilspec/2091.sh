touch foo-bar
touch foo-spam

echo hi > foo-*
echo status=$?

head foo-bar foo-spam
