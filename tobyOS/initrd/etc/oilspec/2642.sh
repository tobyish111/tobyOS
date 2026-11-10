case $SH in dash|mksh|zsh) exit ;; esac

$SH --norc --rcfile /dev/null -c 'echo histfile=${HISTFILE:+yes}'
$SH --norc --rcfile /dev/null -i -c 'echo histfile=${HISTFILE:+yes}'
