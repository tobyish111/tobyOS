shopt -s extglob
mkdir -p eg11
cd eg11
touch {foo,bar,spam}.py
for x in @(fo*|bar).py; do
  echo $x
done

echo ---
declare -a A
A=(zzz @(fo*|bar).py)
echo "${A[@]}"
