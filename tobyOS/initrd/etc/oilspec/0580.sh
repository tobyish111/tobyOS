# zsh and mksh don't do word elision, probably because they do brace expansion
# AFTER variable substitution.
argv.py {X,,Y,}
