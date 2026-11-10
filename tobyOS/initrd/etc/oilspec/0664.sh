bind -q yank | grep -oF '\C-o\C-s\C-h'
echo status=$?

bind '"\C-o\C-s\C-h": yank'
bind -q yank | grep -oF '\C-o\C-s\C-h'
echo status=$?

bind -r "\C-o\C-s\C-h"
bind -q yank | grep -oF '\C-o\C-s\C-h'
echo status=$?
