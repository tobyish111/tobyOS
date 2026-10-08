# `cd` and `pwd` through a link to a directory: the LOGICAL path is the
# default -- $PWD, `pwd`, OLDPWD and `cd ..` keep the link -- and -P, `pwd
# -P` or `set -P` give the physical one. pwd printed the physical directory
# whatever was asked and cd took relative paths from it (logs/b15 cdl).
exec 2>&1
cd /tmp; rm -rf sp292 sp292o; mkdir -p sp292 sp292o/sub; ln -s ../sp292o sp292/dl
cd sp292/dl; echo "1 $(pwd) P=$(pwd -P) L=$(pwd -L) LP=$(pwd -LP) PL=$(pwd -PL) PWD=$PWD"
cd sub; echo "2 $(pwd) P=$(pwd -P) OLDPWD=$OLDPWD"
cd ..; echo "3 $(pwd) PWD=$PWD"
cd ..; echo "4 $(pwd)"
cd -P dl; echo "5 $(pwd) PWD=$PWD"
cd /tmp/sp292; set -P; cd dl; echo "6 $(pwd) PWD=$PWD"; set +P
cd /tmp/sp292; set -o physical; cd dl/sub; echo "7 $(pwd)"; set +o physical
cd /tmp/sp292/dl; cd -; echo "8 back $(pwd)"
cd /tmp/sp292/dl/sub; cd -P ..; echo "9 $(pwd)"
cd /tmp/sp292/dl/sub; cd -L ..; echo "10 $(pwd)"
cd /tmp/sp292/dl; cd ./sub/..; echo "11 $(pwd)"
(cd /tmp/sp292/dl && echo "12 sub $(pwd) $PWD"); echo "12 parent $(pwd)"
pwd extra args; echo "13 st=$?"
pwd -x; echo "14 st=$?"
cd /tmp; rm -rf sp292 sp292o
echo end
