foo=FOO  # should NOT use this

x=foo
ref=x

echo ref=$ref
echo "!ref=${!ref}"

echo 'NOW A NAMEREF'

typeset -n ref
echo ref=$ref
echo "!ref=${!ref}"
