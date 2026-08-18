x='a b'

export y=$x
echo $y

builtin export z=$x
echo $z
