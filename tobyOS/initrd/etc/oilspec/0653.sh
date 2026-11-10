# empty line
mapfile -t lines <<< $'hello\n\nworld'
echo len=${#lines[@]}
#declare -p lines

# initial newline
mapfile -t lines <<< $'\nhello'
echo len=${#lines[@]}
#declare -p lines

# trailing newline
mapfile -t lines <<< $'hello\n'
echo len=${#lines[@]}
#declare -p lines
