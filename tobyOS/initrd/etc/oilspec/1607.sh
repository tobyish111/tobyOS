shopt -s extglob

# this syntax requires extglob in bash!!
# OSH never allows it
g=--@(help|verbose)

quoted='--@(help|verbose)'

[[ --help == $g ]] && echo TRUE
[[ --verbose == $g ]] && echo TRUE
[[ -- == $g ]] || echo FALSE
[[ --help == $q ]] || echo FALSE
[[ -- == $q ]] || echo FALSE
