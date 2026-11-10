exec 2>&1
y=Y; k='$y'
declare -A D
D[$k]=1; D["$k"]=2; echo "1 $(declare -p D)"; D=()
declare D[$k]=3; echo "2 $(declare -p D)"; D=()
declare "D[$k]=4"; echo "3 $(declare -p D)"; D=()
declare 'D[$k]=5'; echo "4 $(declare -p D)"; D=()
w='D[$k]=6'; declare "$w"; echo "5 $(declare -p D)"; D=()
printf -v "D[$k]" %s 7; echo "6 $(declare -p D)"; D=()
read "D[$k]" <<< 8; echo "7 $(declare -p D)"; D=()
eval 'D[$k]=9'; echo "8 $(declare -p D)"; D=()
export D[$k]=10 2>/dev/null; echo "9 $(declare -p D)"; D=()
local_f() { local D2; declare -A D2; local "D2[$k]=11"; declare -p D2; }; local_f
q="'"; D[$q]=12; echo "10 $(declare -p D)"; D=()
D[\$k]=13; echo "11 $(declare -p D)"; D=()
D['$k']=14; echo "12 $(declare -p D)"; D=()
i=1; a=(); declare "a[\$i]=15"; echo "13 $(declare -p a)"
