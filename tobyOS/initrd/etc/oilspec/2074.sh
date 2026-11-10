case $SH in dash|mksh) exit ;; esac

rm -f dbracket dparen for-expr

[[ x = x ]] > dbracket

(( 42 )) > dparen

for ((x = 0; x < 1; ++x)); do
  echo for-expr
done > for-expr

wc -l dbracket dparen for-expr
