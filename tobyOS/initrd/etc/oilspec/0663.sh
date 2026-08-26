bind -X | grep -oF 'emacs|vi'
echo status=$?

bind -m emacs -x '"\C-o\C-s\C-h": echo emacs'
bind -m emacs -X | grep -oF 'emacs'
echo status=$?

bind -m vi -x '"\C-o\C-s\C-h": echo vi'
bind -m vi -X | grep -oF 'vi'
echo status=$?
