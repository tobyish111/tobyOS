# A raw marker byte in the script's own text -- a ^A, ^B or ^D typed into
# a word, quoted or not, or into an array list -- is data, as one that
# an escape makes is (finding 375). tsh took a raw ^A for its argument
# mark and the byte was gone.
exec 2>&1
echo x | od -An -c
v=; echo ${#v}
echo "" XX | od -An -c
case  in ) echo match;; *) echo no;; esac
a=(   ); declare -p a
declare -A B; B=(   ); declare -p B
