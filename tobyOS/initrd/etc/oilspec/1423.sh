# Make the following directory structure. File type and permission bits are
# given on the left.
# [drwxr-xr-x]  _tmp
# +-- [drwxr-xr-x]  bin
# |   \-- [-rwxr-xr-x]  hello
# +-- [drwxr-xr-x]  notbin
# |   \-- [-rw-r--r--]  hello
# \-- [drwxr-xr-x]  dir
#     \-- [drwxr-xr-x]  hello
mkdir -p _tmp/bin
mkdir -p _tmp/bin2
mkdir -p _tmp/notbin
mkdir -p _tmp/dir/hello
printf '#!/usr/bin/env sh\necho hi\n' >_tmp/notbin/hello
printf '#!/usr/bin/env sh\necho hi\n' >_tmp/bin/hello
chmod +x _tmp/bin/hello

DIR=$PWD/_tmp/dir
BIN=$PWD/_tmp/bin
NOTBIN=$PWD/_tmp/notbin

# The command resolution will search the path for matching *files* (not
# directories) WITH the execute bit set.

# Should find executable hello right away and run it
PATH="$BIN:$PATH" hello
echo status=$?

hash -r  # Needed to clear the PATH cache

# Will see hello dir, skip it and then find&run the hello exe
PATH="$DIR:$BIN:$PATH" hello
echo status=$?

hash -r  # Needed to clear the PATH cache

# Will see hello (non-executable) file, skip it and then find&run the hello exe
PATH="$NOTBIN:$BIN:$PATH" hello
echo status=$?
