typeset -A assoc
assoc=(k1 v1 k2 v2 k3 v3)
for k in "${(@k)assoc}"; do
  echo "$k: $assoc[$k]"
done
