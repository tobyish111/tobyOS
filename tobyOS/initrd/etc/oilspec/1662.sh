myarr=(*.ZZ)
echo "${myarr[@]}"
shopt -s failglob
myarr=(*.ZZ)
echo status=$?
