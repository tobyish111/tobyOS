s+=foo
echo s=$s

# bash and mksh agree that this does NOT respect set -u.
# I think that's a mistake, but += is a legacy construct, so let's copy it.

set -u

t+=foo
echo t=$t
t+=foo
echo t=$t
