# THE QUOTED PART OF A ${x-WORD} IN [[ ]] IS LITERAL: on the right of ==
# and != it matches itself, not as a pattern, and on the right of =~ a
# quoted regex part matches literally -- the same as the word written
# without the expansion. tsh flushed the expansion's text with its quotes
# already gone, so ${u:-'a*'} was the pattern a* (finding 111; logs/b18 dbr).
exec 2>&1
t() { if eval "[[ $1 ]]"; then echo "$2 y"; else echo "$2 n"; fi; }
t "ab == \${u:-'a*'}"      d1
t "'a*' == \${u:-'a*'}"    d2
t "ab == \${u:-\"a*\"}"    d3
t "ab == \${u:-a*}"        d4
t "ab == x\${u:-'*'}"      d5
t "ab == \${u:-'a'}*"      d6
t "'a*' == \${u:-a'*'}"    d7
t "ab == \${u:-a'*'}"      d8
t "ab != \${u:-'a*'}"      d9
t "ab =~ \${u:-'a.'}"      r1
t "a. =~ \${u:-'a.'}"      r2
t "ab =~ \${u:-a.}"        r3
t "ab =~ ^\${u:-'a'}.\$"   r4
u=1
t "ab == \${u:+'a*'}"      p1
t "'a*' == \${u:+'a*'}"    p2
t "ab == \${u:+a*}"        p3
unset u
t "ab == \${u:-\${w:-'a*'}}"  n1
t "ab == \${u:-\${w:-a*}}"    n2
t "ab == \${u:-\$'a*'}"       n3
t "'a*' == \${u:-\$'a*'}"     n4
v='a*'
t "ab == \${u:-\$v}"          v1
t "ab == \${u:-\"\$v\"}"      v2
t "ab == \"\${u:-'a*'}\""     q1
t "\"'a*'\" == \"\${u:-'a*'}\"" q2
echo end
