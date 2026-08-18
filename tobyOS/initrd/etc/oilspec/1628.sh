shopt -s extglob
[[ foo == @(foo|bar) ]] && echo rhs
[[ foo == ${unset:-@(foo|bar)} ]] && echo 'rhs arg'
[[ fo == ${unset:-@(foo|bar)} ]] || echo nope
