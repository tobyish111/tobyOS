choice1='help'
choice2='verbose'
[[ --verbose == --@($choice1|$choice2) ]] && echo TRUE
[[ --oops == --@($choice1|$choice2) ]] || echo FALSE
