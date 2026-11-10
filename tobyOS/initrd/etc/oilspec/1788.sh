echo 'echo 1' > my-history

# paper over OSH difference by deleting the ^D line
echo 'history -a
echo 2' | HISTFILE=my-history $SH --rcfile /dev/null -i | sed '/\^D/d'

echo
echo '-- after shell exit --'
cat my-history
