# It's like a single command word.  Parts are joined directly.
var='a b c'
argv.py ${Unset:-A$var " $var"D E F}
