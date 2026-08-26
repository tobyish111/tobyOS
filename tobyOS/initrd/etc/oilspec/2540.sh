case $SH in dash) exit ;; esac

echo '+ ' /"${array[@]+foo}"/
echo '+:' /"${array[@]:+foo}"/
echo

typeset -a array
array=()

echo '+ ' /"${array[@]+foo}"/
echo '+:' /"${array[@]:+foo}"/
echo

array=('')

echo '+ ' /"${array[@]+foo}"/
echo '+:' /"${array[@]:+foo}"/
echo

array=(spam eggs)

echo '+ ' /"${array[@]+foo}"/
echo '+:' /"${array[@]:+foo}"/
echo



# Bash 2.0..4.4 has a bug that "${a[@]:-xxx}" produces an empty string.  It
# seemed to consider a[@] and a[*] are non-empty when there is at least one
# element even if the element is empty.  This was fixed in Bash 5.0.
#
# ## BUG bash STDOUT:
# +  //
# +: //
#
# +  //
# +: //
#
# +  /foo/
# +: /foo/
#
# +  /foo/
# +: /foo/
#
# ## END
