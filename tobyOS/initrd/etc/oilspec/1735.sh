# bash is OK with this; dash isn't.  Should be a parse error.
cat <<$(a)
here
$(a)
