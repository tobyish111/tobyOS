bind -m badmap -p; echo "st=$?"
bind -q nosuchfn; echo "st=$?"
bind -u nosuchfn; echo "st=$?"
bind 'set nosuchvar on'; echo "st=$?"
bind '"\C-x'; echo "st=$?"
bind 'nosep'; echo "st=$?"
bind '"\C-o": nosuchfunc'; echo "st=$?"; bind -q yank
bind '"\C-o": yank'; echo "st=$?"; bind -q yank; bind -P | grep -w yank
bind -r '\C-o'; echo "st=$?"; bind -q yank
bind -r '\C-z'; echo "st=$?"
bind -f /nonexistent/file; echo "st=$?"
bind -x 'nodq: cmd'; echo "st=$?"
bind -x '"\C-o" cmd'; echo "st=$?"
bind -x '"\C-o": "unterminated'; echo "st=$?"
bind -x '"\C-t": echo hi'; bind -X; bind -m vi-insert -X; echo "st=$?"
bind 'set bell-style none'; bind 'set completion-query-items 50'; bind 'set editing-mode vi'; bind -v | grep -E 'bell|query|editing|keymap'; bind 'set editing-mode emacs'
bind 'set comment-begin "## "'; bind -V | grep comment
bind '"\C-xq": "macro text"'; bind -s; bind -S; echo "st=$?"
bind 'Control-q: yank'; bind -q yank
bind 'Meta-q: yank'; bind -q yank
bind -l | wc -l
bind -m vi -q yank
bind -z; echo "st=$?"
bind --help | head -3; echo "st=$?"
bind -p | head -2
bind 'set convert-meta off'; bind -q yank; bind 'set convert-meta on'
bind -u yank; bind -q yank; echo "st=$?"
