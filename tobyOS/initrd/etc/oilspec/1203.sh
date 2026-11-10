# mksh and bash have different line numbers in this case
#trap 'echo line=$LINENO' ERR
trap 'echo line=$LINENO' ERR

# it's run for the last 'false'
false | false | false

{ echo pipeline; false; } | false | false

# it's never run here
! true
! false
