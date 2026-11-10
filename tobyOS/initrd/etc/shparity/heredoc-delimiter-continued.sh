# A here-document delimiter that a backslash-newline continues is the joined
# word -- `<<\EOT\` then `4` is the delimiter EOT4 -- inside $( ) as outside;
# tsh kept the backslash in the word and read to the end of the file
# (finding 440).
exec 2>&1
x=$( cat <<\EOT\
4
d \
g
EOT4
)
echo "$x"
x=$( cat <<EOT\
5
hi
EOT5
)
echo "[$x]"
x=$( cat <<\
EOT
there
EOT
)
echo "[$x]"
