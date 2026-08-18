declare -a a=(ale bean)
echo first=${!a}

ale=zzz
echo first=${!a}
