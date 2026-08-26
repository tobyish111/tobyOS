shopt -s extglob
pat='--@(help|verbose|no-@(long|short)-option)'
[[ --no-long-option == $pat ]] && echo TRUE
[[ --no-short-option == $pat ]] && echo TRUE
[[ --help == $pat ]] && echo TRUE
[[ --oops == $pat ]] || echo FALSE
