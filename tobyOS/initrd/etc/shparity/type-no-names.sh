# `type` with no name says nothing and returns 0, as bash's does
# (type.tests); tsh printed a usage of its own on stdout, status 1. A bad
# option adds bash's usage line on stderr, status 2.
type; echo "rc=$?"
type -r ls 2>/dev/null; echo "rc=$?"
type -- ; echo "rc=$?"
