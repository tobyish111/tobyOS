# Inspired by https://blog.sunfishcode.online/bugs-in-hello-world/

echo hi > /dev/full
echo status=$?

printf '%s\n' hi > /dev/full
echo status=$?
