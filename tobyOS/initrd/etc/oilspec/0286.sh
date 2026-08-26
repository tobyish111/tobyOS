case $SH in mksh) exit ;; esac

_set_COMPREPLY() {
  COMPREPLY=({0..9})
  unset -v 'COMPREPLY[2]' 'COMPREPLY[4]' 'COMPREPLY[6]'
}

compgen -F _set_COMPREPLY
