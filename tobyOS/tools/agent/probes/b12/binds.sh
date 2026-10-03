exec 2>/dev/null
for m in emacs emacs-standard emacs-meta emacs-ctlx vi vi-move vi-command vi-insert; do
  echo "=====MAP $m"; bind -m $m -p
done
echo "=====P"; bind -P
echo "=====V"; bind -v
echo "=====VV"; bind -V
echo "=====L"; bind -l
echo "=====S"; bind -s
echo "=====SS"; bind -S
echo "=====X"; bind -X
echo "=====VIS"; bind -m vi-insert -s; bind -m vi-command -s
echo "=====END"
