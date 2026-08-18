echo bytes1
not_utf8=$(python2 -c 'print("\xce\xce")')

printf '%x\n' \'$not_utf8
printf '%u\n' \'$not_utf8
printf '%o\n' \'$not_utf8
echo

echo bytes2
not_utf8=$(python2 -c 'print("\xbc\xbc")')
printf '%x\n' \'$not_utf8
printf '%u\n' \'$not_utf8
printf '%o\n' \'$not_utf8
echo

# Copied from data_lang/utf8_test.cc

echo overlong2
overlong2=$(python2 -c 'print("\xC1\x81")')
printf '%x\n' \'$overlong2
printf '%u\n' \'$overlong2
printf '%o\n' \'$overlong2
echo

echo overlong3
overlong3=$(python2 -c 'print("\xE0\x81\x81")')
printf '%x\n' \'$overlong3
printf '%u\n' \'$overlong3
printf '%o\n' \'$overlong3
echo
