#shopt -s verbose_errexit

# This turns on command_sub_errexit and fails
var x = $(echo bad; false)
echo 'unreachable'

pp test_ (x)
