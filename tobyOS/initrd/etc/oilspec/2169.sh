# Hm semicolon is still an operator in bash
$SH <<'EOF'
[[ ';' =~ ; ]] && echo semi
EOF
echo semi=$?

$SH <<'EOF'
[[ ';' =~ (;) ]] && echo semi paren
EOF
echo semi paren=$?

echo

$SH <<'EOF'
[[ '&' =~ & ]] && echo amp
EOF
echo amp=$?

# Oh I guess this is not a bug?  regcomp doesn't reject this trivial regex?
$SH <<'EOF'
[[ '|' =~ | ]] && echo pipe1
[[ 'a' =~ | ]] && echo pipe2
EOF
echo pipe=$?

$SH <<'EOF'
[[ '|' =~ a| ]] && echo four
EOF
echo pipe=$?

# This is probably special because > operator is inside foo [[ a > b ]]
$SH <<'EOF'
[[ '<>' =~ <> ]] && echo angle
EOF
echo angle=$?

# Bug: OSH allowed this!
$SH <<'EOF'
[[ $'a\nb' =~ a
b ]] && echo newline
EOF
echo newline=$?
