IFS=x
v=
echo "${v:-AxBxC}"
echo "${v:-AxBxC}"x  # <-- osh failed this
echo ${v:-AxBxC}
echo ${v:-AxBxC}x
echo ${v:-"AxBxC"}
echo ${v:-"AxBxC"}x
echo "${v:-"AxBxC"}"
echo "${v:-"AxBxC"}"x
