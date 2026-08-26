a=(x y)
PWD=1
ref='a[~+]'
echo ${!ref}

# Bash 4.4 had a bug, which was fixed in Bash 5.0.
#
# ## BUG bash status: 0
# ## BUG bash STDOUT:
# y
# ## END
