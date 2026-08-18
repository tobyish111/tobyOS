shopt -s extglob
mkdir -p eg1
touch eg1/_ eg1/_One eg1/_OneOne eg1/_TwoTwo eg1/_OneTwo
echo eg1/_*(One|Two)
