# copied directly from git completion - 2024-04

if false; then
  unset ${(M)${(k)parameters[@]}:#__gitcomp_builtin_*} 2>/dev/null
fi
echo status=$?
