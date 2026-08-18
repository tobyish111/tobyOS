typeset -A assoc
assoc=(k1 v1 k2 v2 k3 v3)
for k v ("${(@kv)assoc}"); do
  echo "$k: $v"
done
