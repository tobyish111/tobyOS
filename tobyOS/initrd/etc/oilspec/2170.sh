$SH <<'EOF'
[[ '|' =~ '|' ]] && echo sq
EOF
echo sq=$?

$SH <<'EOF'
[[ '|' =~ "|" ]] && echo dq
EOF
echo dq=$?

$SH <<'EOF'
[[ '|' =~ $'|' ]] && echo dollar-sq
EOF
echo dollar-sq=$?

$SH <<'EOF'
[[ '|' =~ $"|" ]] && echo dollar-dq
EOF
echo dollar-dq=$?
