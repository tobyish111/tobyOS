IFS=x; X=abxcd; echo ${X/bxc/g}

X=a=\"\$a\"; echo ${X//a/{x,y,z}}
