# bash-completion relies on this, so we're making it match bash.
# zsh understandably differs.
[[ '[]' =~ \[\] ]] && echo true

# Another way to write this.
pat='\[\]'
[[ '[]' =~ $pat ]] && echo true
