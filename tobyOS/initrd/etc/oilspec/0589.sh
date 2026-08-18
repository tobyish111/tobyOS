HOME=/home/bob

# Command
echo ~{/src,root}

# Loop

for x in ~{/src,root}; do
  echo -- $x
done

# Array

a=(~{/src,root})

for y in "${a[@]}"; do
  echo "== $y"
done
