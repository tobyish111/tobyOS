paths=$(tr '\n' ':' | sed -e 's/:$//' <<EOPATHS
/foo
/bar
/baz
EOPATHS
)
echo "$paths"
