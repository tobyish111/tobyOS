[[ 1+2 -eq 3 ]] && echo true
expr='1+2'
[[ $expr -eq 3 ]] && echo true  # must be dynamically parsed
