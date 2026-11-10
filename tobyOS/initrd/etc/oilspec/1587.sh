shopt -s extglob
mkdir -p 2
touch 2/{aa,ab,ac,ba,bb,bc,ca,cb,cc}
echo 2/!(b)@(b|c)
echo 2/!(b)?@(b|c)  # wildcard in between
echo 2/!(b)a@(b|c)  # constant in between
