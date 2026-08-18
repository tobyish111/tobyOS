case $SH in dash|zsh) exit ;; esac

HOME=/home/bar
declare -a a
a[0]=foo:~
echo ${a[0]}

declare -A A
A['x']=foo:~
echo ${A['x']}
