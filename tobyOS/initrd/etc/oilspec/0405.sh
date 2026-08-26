f() {
  declare -g G=42
  declare L=99

  declare -Ag dict
  dict["foo"]=bar

  declare -A localdict
  localdict["spam"]=Eggs

  # For bash-completion
  eval 'declare -Ag ev'
  ev["ev1"]=ev2
}
f
argv.py "$G" "$L"
argv.py "${dict["foo"]}" "${localdict["spam"]}"
argv.py "${ev["ev1"]}"
