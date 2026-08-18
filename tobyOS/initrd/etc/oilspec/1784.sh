cat >myrc <<'EOF'

# from test/process-table-portable.sh
PS_COLS='pid,ppid,pgid,sid,tpgid,comm'

show-shell-state() {
  local prefix=$1

  echo -n "$prefix: "

  echo "pid = $$"

  # Hm TPGID has changed in both OSH and bash
  # I guess that's because because ps itself becomes the leader of the process
  # group

  ps -o $PS_COLS $$
}

show-shell-state myrc


EOF

$SH --rcfile myrc -i -c 'show-shell-state main'


# No assertions
# TODO: spec test framework should be expanded to properly support these
# comparisons.
# The --details flag is useful
