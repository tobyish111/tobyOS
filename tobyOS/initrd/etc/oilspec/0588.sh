# The brace expansion happens FIRST.  After that, the second token has tilde
# FIRST, so it gets expanded.  The first token has an unexpanded tilde, because
# it's not in the leading position.

HOME=/home/bob

# Command

echo {foo~,~}/bar

# Loop

for x in {foo~,~}/bar; do
  echo -- $x
done

# Array

a=({foo~,~}/bar)

for y in "${a[@]}"; do
  echo "== $y"
done
