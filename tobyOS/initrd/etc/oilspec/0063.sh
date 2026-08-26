dyn=x

typeset s${dyn}+=foo
echo sx=$sx

# bash and mksh agree that this does NOT respect set -u.
# I think that's a mistake, but += is a legacy construct, so let's copy it.

set -u

typeset t${dyn}+=foo
echo tx=$tx
typeset t${dyn}+=foo
echo tx=$tx
