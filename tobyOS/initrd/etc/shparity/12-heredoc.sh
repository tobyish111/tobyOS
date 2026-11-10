# Here-documents, expanded and quoted-delimiter (literal). Fed into a function
# so the case needs no external program: a here-doc is just stdin, and a shell
# that only wires it up for external commands fails exactly here.
lines() { while read l; do echo "hd:$l"; done; }

name=world
lines <<EOF
hello $name
literal line
EOF

lines <<'EOF'
no $name expansion
$(echo nor this)
EOF

lines <<END
different delimiter
END

echo heredoc-done
