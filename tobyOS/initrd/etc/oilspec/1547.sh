# from http://mywiki.wooledge.org/BashFAQ/105, this changed between versions.
# ash says that 'i++' is not found, but it doesn't exit.  I guess this is the 
# subshell problem?
set -o errexit
i=0
(( i++ ))
echo done
