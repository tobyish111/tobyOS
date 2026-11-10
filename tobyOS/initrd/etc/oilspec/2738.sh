# bash, zsh, and yash support unicode in IFS, but dash/mksh/ash don't.

# for zsh, though we're not testing it here
setopt SH_WORD_SPLIT

x=çx IFS=ç
printf "<%s>\n" $x
