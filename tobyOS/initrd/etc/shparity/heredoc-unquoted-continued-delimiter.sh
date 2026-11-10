# In a here-document with an unquoted delimiter a backslash-newline joins
# the body's lines before the delimiter is looked for: `EO\` then `F` is the
# line EOF and ends it, and `END\` then `END` is ENDEND, a line of the body
# (finding 446).
exec 2>&1
cat << EOF
hi
EO\
F
echo after1
cat <<END
hello
END\
END
END
cat <<END
hello\
END
END
cat <<'EOF'
kept\
EO\
F
EOF
