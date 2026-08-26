# All shells agree on this, but it's very confusing behavior.
FOO=foo readonly v=$(printenv.py FOO)
echo "v=$v"

# bash has probems here:
FOO=foo readonly v2=$FOO
echo "v2=$v2"
