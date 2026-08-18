x=fooz
pat='[z-a]'  # Invalid range.  Other shells don't catch it!
#pat='[a-y]'
echo ${x//$pat}
echo status=$?
