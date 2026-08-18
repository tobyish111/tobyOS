set -x
printf 'aaaa' > /dev/null 2> test_osh
set +x
cat test_osh
