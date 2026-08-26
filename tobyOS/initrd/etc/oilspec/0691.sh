echo '1' > $TMP/testr_yes
echo '2' > $TMP/testr_no
chmod -r $TMP/testr_no  # remove read permission
test -r $TMP/testr_yes && echo 'yes'
test -r $TMP/testr_no || echo 'no'
