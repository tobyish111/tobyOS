case $SH in bash|mksh|ash) exit ;; esac

# seems the same

shopt -s ysh:upgrade

proc handler {
  echo err
}

if test -f /nope { echo file exists }

trap handler ERR

if test -f /nope { echo file exists }

false || true  # not run for the first part here
false
