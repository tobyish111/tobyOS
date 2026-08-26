case $SH in dash|mksh) exit ;; esac

shopt -s lastpipe

seq 3 | echo last=$_

echo pipeline=$_

( echo subshell=$_ )
echo done=$_


# very weird semantics for zsh!
