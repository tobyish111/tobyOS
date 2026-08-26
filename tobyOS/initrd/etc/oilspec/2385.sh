# http://landley.net/notes.html#24-06-2020

# Fix these
case $SH in dash|mksh|zsh) exit ;; esac

xx() { echo "${*@Q}";}; xx a b c d
xx() { echo "${@@Q}";}; xx a b c d
