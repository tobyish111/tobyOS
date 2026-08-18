set -- one two three

ref='#'
echo ref=$ref
typeset -n ref
echo ref=$ref


# mksh does respect it!!  Gah.
