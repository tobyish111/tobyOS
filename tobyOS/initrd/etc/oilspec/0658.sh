# silly workaround for spec test format - change # comment to %
bind -p | grep vi-subst | sed 's/^#/%/'
echo

bind -P | grep vi-subst
