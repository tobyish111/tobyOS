# += through a reference to an element is that element's +=, with its
# array's attributes: an integer array adds (finding 341). tsh appended the
# text, 41.
{
( declare -ai a; a[0]=4; declare -n b='a[0]'; b+=1; declare -p a; declare b+=1; declare -p a )
( f() { local -ai a=(0 12); local -n b=a[1]; b+=4; echo "$b"; declare -p a; }; f )
( declare -i n=3; declare -n m=n; m+=4; declare -p n )
( declare -a s=(x y); declare -n t='s[1]'; t+=z; declare -p s )
} 2>&1
