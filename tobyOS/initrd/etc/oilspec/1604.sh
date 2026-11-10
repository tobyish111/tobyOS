shopt -s extglob

rm -f _failglob/*
mkdir -p _failglob
cd _failglob

shopt -s failglob
echo @(*)
echo status=$?

touch foo
echo @(*)
echo status=$?
