setopt SH_WORD_SPLIT  # for zsh

set -- 'a b' c ''

# default IFS
argv.py '  $*  '  $*
argv.py ' "$*" ' "$*"
argv.py '  $@  '  $@
argv.py ' "$@" ' "$@"
echo

IFS=''
argv.py '  $*  '  $*
argv.py ' "$*" ' "$*"
argv.py '  $@  '  $@
argv.py ' "$@" ' "$@"
echo

IFS=zx
argv.py '  $*  '  $*
argv.py ' "$*" ' "$*"
argv.py '  $@  '  $@
argv.py ' "$@" ' "$@"


# zsh disagrees on
# - $@ with default IFS an
# - $@ with IFS=zx
