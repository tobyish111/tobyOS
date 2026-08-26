while cat <<E1 && cat <<E2
1
E1
2
E2
do
  cat <<E3
3
E3
  break
done
