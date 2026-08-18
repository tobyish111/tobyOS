shopt -s extglob
[[ '' == @(foo|bar) ]] || echo FALSE
[[ '' == @(foo||bar) ]] && echo TRUE
