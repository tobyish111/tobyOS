# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p48 in C:\t\b12)
exec 2>&1
[[ -n ( ]]; echo "s=$?"
