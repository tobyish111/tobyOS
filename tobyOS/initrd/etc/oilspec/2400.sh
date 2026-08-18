# Should YSH be different?  It would be nice.
# We would have to validate all Lit_Chars tokens, and the like.
#
# The logical place to put that would be in osh/word_parse.py where we read
# single and double quoted strings.  Although there might be a global lexer
# hack for Id.Lit_Chars tokens.  Would that catch here docs though?

# Test all the lexing contexts
cat >unicode.sh << 'EOF'
echo μ 'μ' "μ" $'μ'
EOF

# Show that all lexer modes recognize unicode sequences
#
# Oh I guess we need to check here docs too?

#$SH -n unicode.sh

$SH unicode.sh

# Trim off the first byte of mu
sed 's/\xce//g' unicode.sh > not-unicode.sh

echo --
$SH not-unicode.sh | od -A n -t x1



# dash and ash don't support $''
