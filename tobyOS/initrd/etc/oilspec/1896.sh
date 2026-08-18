#shopt -s verbose_errexit

# I don't understand why this doesn't fail
var x = $(echo one; false; echo two)
echo 'unreachable'

pp test_ (x)
