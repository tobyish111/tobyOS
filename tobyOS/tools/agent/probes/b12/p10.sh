x='hello World foo-bar BAZ9q'
echo "${x^}" "${x^^}" "${x,}" "${x,,}" "${x~}" "${x~~}"
echo "${x^^[lo]}" "${x,,[A-Z]}" "${x~~[a-m]}" "${x^o}" "${x^h}" "${x^^?}"
a=(abc DEF gHi); echo "${a[@]^}" "${a[@]~~}" "${a[*]^^[a-c]}" "${a[1],}"
set -- one Two; echo "${@^}" "${*~~}"
s='ab^cd^]x'; echo ${s//[^]]/z} ${s//[!x]/Q} ${s#[^]]} ${s%[]x]} ${s//]/R}
p='[]'; echo ${s//$p/E}
