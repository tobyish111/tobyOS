cd $REPO_ROOT

# like above test case, but we source

# bash location doesn't make sense:
# - It says 'source' happens at line 1 of bash-source-pushtemp.  Well I think
# - It really happens at line 2 of '-c' !    I guess that's to line up
#   with the 'main' frame

$SH -c 'true;
source spec/testdata/bash-source-pushtemp.sh'
