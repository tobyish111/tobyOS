rm -f myhist
export HISTFILE=myhist
echo 'echo hist1; echo hist2' | $SH --norc -i

if test -n "$BASH_VERSION"; then
  echo '^D'  # match OSH for now
fi

cat myhist
# cat ~/.config/oil/history_osh
