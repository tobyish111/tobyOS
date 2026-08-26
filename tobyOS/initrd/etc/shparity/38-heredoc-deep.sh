# POSIX 2.7.4 -- here-documents.
# Expansion happens in the body unless the delimiter is quoted; <<- strips
# LEADING TABS ONLY (not spaces), which is why the indented lines below use
# real tab characters.
lines() { while read l; do echo "hd:$l"; done; }

name=world
lines <<EOF
hello $name
$(echo substituted)
$((1+1))
literal text
EOF

lines <<'EOF'
no $name expansion
$(echo nor this)
$((1+1))
EOF

lines <<"EOF"
also $name literal
EOF

lines <<-EOF
	stripped tab
	$name here too
EOF

lines <<END
different delimiter
END

count=0
while read x; do count=$((count+1)); done <<EOF
l1
l2
l3
EOF
echo "count=$count"
echo end
