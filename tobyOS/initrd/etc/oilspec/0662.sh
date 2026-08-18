bind -X | grep -oF '\C-o\C-s\C-h'
echo status=$?

bind -x '"\C-o\C-s\C-h": echo foo'
bind -X | grep -oF '\C-o\C-s\C-h'
bind -X | grep -oF 'echo foo'
echo status=$?
