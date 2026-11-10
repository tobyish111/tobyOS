# In a here-document with a quoted delimiter inside $( ), a line ending in a
# backslash is a line of the body, kept as it is; with an unquoted delimiter
# the lines join, in $( ) and outside (finding 439).
exec 2>&1
x=$(cat <<'EOT'
d \
g
EOT
)
echo "$x"
x=$(cat <<EOT
e \
h
EOT
)
echo "$x"
cat <<'EOT'
f \
i
EOT
