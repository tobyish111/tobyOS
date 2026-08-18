case $SH in dash|zsh|mksh) exit ;; esac  # NOT IMPLEMENTED

mkdir -p read0
cd read0
rm -f *

touch a\\b\\c\\d  # -r is necessary!

find . -type f -a -print0 | { read -r -d ''; echo "[$REPLY]"; }
