$SH -c '
echo hi > file$(( 42 / 0 )) in
echo inside=$?
'
echo outside=$?



# bash makes the command fail


# bash makes the command fail
