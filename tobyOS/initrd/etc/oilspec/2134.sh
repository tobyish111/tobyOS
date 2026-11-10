set -C  # noclobber
set -e  # errexit (raise any redirection errors)

# Each redirect to /dev/null should succeed
echo a  >  /dev/null  # trunc, write stdout
echo a &>  /dev/null  # trunc, write stdout and stderr
echo a  >> /dev/null  # append, write stdout
echo a &>> /dev/null  # append, write stdout and stderr
echo a  >| /dev/null  # ignore noclobber, trunc, write stdout
