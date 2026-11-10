shopt -s expand_aliases  # bash requires this
alias hi='e_ hello world'
alias e_='echo __'
hi   # first hi is expanded to echo hello world; then echo is expanded.  gah.
