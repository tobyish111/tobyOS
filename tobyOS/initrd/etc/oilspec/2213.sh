show() {
  shopt -o -p | egrep 'emacs$|vi$'
  echo ___
};
show

set -o emacs
show

set -o vi
show
