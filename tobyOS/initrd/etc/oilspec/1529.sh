case $SH in bash|dash|ash|mksh) exit ;; esac

shopt --set oil:upgrade

shopt --unset errexit {
  echo hi
}

proc p {
  echo p
}

shopt --unset errexit {
  p
}
