# In a UTF-8 locale a pattern or a string that is no valid text is matched
# byte by byte: a lone continuation byte matches inside a character --
# tsh matched characters only, where the byte is none (finding 496).
exec 2>&1
export LANG=en_US.UTF-8
euro=$'\342\202\254'; o202=$'\202'
case $euro in *$o202*) echo bytematch ;; *) echo mbchar ;; esac
echo "${euro##*$o202}" | od -b | head -1
echo "${euro%$o202*}" | od -b | head -1
echo "${euro/$o202/X}" | od -c | head -1
[[ $euro == *$o202* ]] && echo dbr-bytes || echo dbr-chars
case $euro in ?) echo one-char ;; *) echo not-one ;; esac
