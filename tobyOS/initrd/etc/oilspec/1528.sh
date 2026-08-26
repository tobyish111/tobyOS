set -o errexit

if echo tmp_contents > $(echo tmp); then
  echo 2
fi
cat tmp
