# bash 4.4 gives empty string, but I feel like this could be an error
echo undef=${!undef-'default'}
echo undef=${!undef}

set -u
echo NOUNSET
echo undef=${!undef-'default'}
echo undef=${!undef}


# Bash 4.4 had been generating an empty string, but it was fixed in Bash 5.0.
#
# ## BUG bash STDOUT:
# undef=default
# undef=
# NOUNSET
# undef=default
# ## END
