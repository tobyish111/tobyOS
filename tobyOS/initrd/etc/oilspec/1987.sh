# zsh is very similar to bash, but don't bother with the assertions
case $SH in bash*|zsh) exit ;; esac

shopt --set parse_at

f() {
  cat <(seq 1; exit 1) | {
    cat <(seq 2; exit 2) <(seq 3; exit 3)

    # 2022-11 workaround for race condition: sometimes we get pipeline=141 4
    # instead of pipeline=0 4, which means that the first 'cat' got SIGPIPE.
    # If we make this part of the pipeline take longer, then 'cat' should have
    # a chance to finish.

    sleep 0.01

    (exit 4)
  }
  echo status=$?
  echo process_sub @_process_sub_status
  echo pipeline @_pipeline_status
  echo __
}

f
