shopt -s extglob
[[ foo =~ ^@(foo|bar)$ ]] || echo FALSE
