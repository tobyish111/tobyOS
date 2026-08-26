case $SH in yash) exit ;; esac  # no echo -n

setopt SH_WORD_SPLIT  # for zsh

set -- 'a b' c ''

# default IFS
echo -n '  $*  ';  for i in  $*;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$*" ';  for i in "$*"; do echo -n ' '; echo -n -$i-; done; echo
echo -n '  $@  ';  for i in  $@;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$@" ';  for i in "$@"; do echo -n ' '; echo -n -$i-; done; echo
echo

IFS=''
echo -n '  $*  ';  for i in  $*;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$*" ';  for i in "$*"; do echo -n ' '; echo -n -$i-; done; echo
echo -n '  $@  ';  for i in  $@;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$@" ';  for i in "$@"; do echo -n ' '; echo -n -$i-; done; echo
echo

IFS=zx
echo -n '  $*  ';  for i in  $*;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$*" ';  for i in "$*"; do echo -n ' '; echo -n -$i-; done; echo
echo -n '  $@  ';  for i in  $@;  do echo -n ' '; echo -n -$i-; done; echo
echo -n ' "$@" ';  for i in "$@"; do echo -n ' '; echo -n -$i-; done; echo
