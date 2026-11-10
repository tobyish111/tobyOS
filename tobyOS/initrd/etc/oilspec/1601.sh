shopt -s extglob
echo @(__nope__)

# OSH has glob quoting here
echo @(__nope__*|__nope__?|'*'|'?'|'[:alpha:]'|'|')

if test $SH != osh; then
  exit
fi

# OSH has this alias for @()
echo ,(osh|style)
