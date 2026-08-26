foo_complete() {
  # first, cur, prev
  argv.py argv "$@"
  argv.py COMP_WORDS "${COMP_WORDS[@]}"
  argv.py COMP_CWORD "${COMP_CWORD}"
  argv.py COMP_LINE "${COMP_LINE}"
  argv.py COMP_POINT "${COMP_POINT}"
  #return 124
  COMPREPLY=(one two three)
}
compgen -F foo_complete foo a b c
