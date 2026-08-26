x='foo()' 
echo 1 ${x%*\(\)}
echo 2 ${x%%*\(\)}
echo 3 ${x#*\(\)}
echo 4 ${x##*\(\)}
