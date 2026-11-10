# POSIX collating symbols and equivalence classes in a bracket expression,
# and bash's [:ascii:] class: [[.a.]], [[.hyphen.]], [[=b=]] matched
# nothing in tsh (bash's posixpat.tests; finding 291).
m() { case $1 in $2) echo "$1 ~ $2: yes";; *) echo "$1 ~ $2: no";; esac; }
m a '[[.a.]]'
m - '[[.hyphen.]-9]'
m p '[[.a.]-[.z.]]'
m - '[[.-.]]'
m ' ' '[[.space.]]'
m ' ' '[[.grave-accent.]]'
m 4 '[[.-.]-9]'
m c '[[.yyz.]-[.z.]]'
m c '[[.yyz.][.a.]-z]'
m p '[[.a.]-[.Z.]]'
m p '[[.a.]-[.zz.]p]'
m abc '[[.cb.]a-Za]*'
m "$(printf '\t')" '[[.space.][.tab.][.newline.]]'
m abc '[[:alpha:]][[=b=]][[:ascii:]]'
m abc '[[:alpha:]][[=B=]][[:ascii:]]'
m a '[[=b=]'
[[ b == [[=b=]] ]]; echo "dbr=$?"
v=xay; echo "${v//[[.a.]]/_}"
