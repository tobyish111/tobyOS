shopt -s expand_aliases
alias e_='echo '
alias one='ONE '
alias two='TWO '
alias three='THREE'  # no trailing space
e_ one \
  two one \
  two three two \
  one
