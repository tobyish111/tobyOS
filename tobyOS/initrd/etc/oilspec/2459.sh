# found via test/parse-errors.sh

x='slash / brace } hi'
echo 'ambiguous:' ${x///}

echo 'quoted:   ' ${x//'/'}

# Wow we have all combination here -- TERRIBLE
