x='[foo]'
echo ${x//[\[z]/<}  # the right way to do it
echo ${x//[\]z]/>}
echo ${x//[[z]/<}  # also accepted
echo ${x//[]z]/>}
