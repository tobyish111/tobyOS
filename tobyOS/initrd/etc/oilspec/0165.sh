s='12 34'
echo '12 34' $(( s[0] )) $(( s[1] ))
echo status=$?




# bash prints an error, but doesn't fail
