v='[\f]'
x='\f'
echo ${v/"$x"/_}

# mksh and zsh differ on this case, but this is consistent with the fact that
# \f as a glob means 'f', not '\f'.  TODO: Warn that it's a bad glob?
# The canonical form is 'f'.
echo ${v/$x/_}

echo ${v/\f/_}
echo ${v/\\f/_}
