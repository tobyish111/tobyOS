# FOO is not respected here either.
export FOO=foo v=$(printenv.py FOO)
echo "v=$v"
