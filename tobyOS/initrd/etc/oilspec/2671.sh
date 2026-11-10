case $SH in dash|mksh) exit ;; esac

setopt shwordsplit  # for ZSH

x='with spaces'
: $x
echo $_
