mkdir -p _tmp
shopt -s expand_aliases || true  # bash

alias ll='ls -l'

touch _tmp/date
chmod +x _tmp/date
PATH=_tmp:/bin

normalize() {
  # ignore quotes and backticks
  # bash prints a left backtick
  quotes='"`'\'
  sed \
    -e "s/[$quotes]//g" \
    -e 's/shell function/function/' \
    -e 's/is aliased to/is an alias for/'
}

type ll date | normalize

# Note: both procs and funcs go in var namespace?  So they don't respond to
# 'type'?
