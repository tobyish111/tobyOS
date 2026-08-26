# GAP 2: "$@" expands to one word per parameter, even though it is quoted.
# This is what makes a wrapper script able to forward arguments containing
# spaces, so the cases with embedded spaces are the ones that matter.
show() { echo "n=$# 1=[$1] 2=[$2] 3=[$3]"; }

set -- one two three
show "$@"
show "$*"
show $@
show $*

set -- "a b" c
show "$@"
show "$*"

set -- solo
show "$@"

# Zero parameters: "$@" must contribute NO words, not one empty one.
set --
show "$@"
echo "argc=$#"

# Forwarding through two levels is the real-world shape.
inner() { echo "inner n=$# 1=[$1] 2=[$2]"; }
outer() { inner "$@"; }
outer "x y" z

set -- 1 2 3
echo "star=[$*] at=[$@]"
echo dollarat-done
