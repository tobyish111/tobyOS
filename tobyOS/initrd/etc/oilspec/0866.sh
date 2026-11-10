mkdir -p _tmp/shell
mkdir -p _tmp/dir/hello.sh
printf 'echo hi' >_tmp/shell/hello.sh

DIR=$PWD/_tmp/dir
SHELL=$PWD/_tmp/shell

# Should find the file hello.sh right away and source it
PATH="$SHELL:$PATH" . hello.sh
echo status=$?

# Should fail because hello.sh cannot be found
PATH="$DIR:$SHELL:$PATH" . hello.sh
echo status=$?
