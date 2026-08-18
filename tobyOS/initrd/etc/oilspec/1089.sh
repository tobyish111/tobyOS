case $SH in dash|ash|zsh) exit ;; esac

echo $'a\nb\nc' > $TMP/read-lines.txt

read -N 3 out < $TMP/read-lines.txt
echo "$out"
