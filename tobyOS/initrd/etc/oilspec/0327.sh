# 2024-06 - bash 5.2 and mksh now match, bash 4.4 differed.
# Could change OSH
# zsh agrees with OSH, but it fails most test cases
# 2025-01 We changed OSH.

single=('')
argv.py ${single[@]:-none} x "${single[@]:-none}"
