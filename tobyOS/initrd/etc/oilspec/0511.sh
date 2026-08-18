case $SH in zsh|mksh|ash|dash|yash) exit ;; esac

alias echo=false

function f {
  shopt -u expand_aliases
  eval -- "$1"
  shopt -s expand_aliases
}

f 'echo hello'
