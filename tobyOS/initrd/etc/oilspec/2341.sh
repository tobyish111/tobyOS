shopt --set strict:all

# This differs from what it means in a process
FOO=bar eval 'echo FOO=$FOO'
echo FOO=$FOO
