case $SH in dash|bash|mksh|ash) exit ;; esac

set -o errexit
shopt --set parse_brace command_sub_errexit verbose_errexit || true

rm -f BAD

try {
  echo $(date %d) $(touch BAD)
}
if ! test -f BAD; then  # should not exist
  echo OK
fi
