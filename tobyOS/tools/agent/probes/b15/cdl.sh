# cdl.sh: cd and pwd through a symbolic link to a directory -- logical
# (-L, the default) against physical (-P, set -P), $PWD and $OLDPWD, `cd ..`
# from inside the link, and pwd's options (item 3: links are real now).
exec 2>&1
cd /tmp; rm -rf cl clo; mkdir -p cl clo/sub; ln -s ../clo cl/dl
cd cl/dl; echo "1 pwd=$(pwd) P=$(pwd -P) L=$(pwd -L) LP=$(pwd -LP) PL=$(pwd -PL) PWD=$PWD"
cd sub; echo "2 pwd=$(pwd) P=$(pwd -P) OLDPWD=$OLDPWD"
cd ..; echo "3 pwd=$(pwd) PWD=$PWD"
cd ..; echo "4 pwd=$(pwd)"
cd -P dl; echo "5 pwd=$(pwd) PWD=$PWD"
cd /tmp/cl; set -P; cd dl; echo "6 pwd=$(pwd) PWD=$PWD"; set +P
cd /tmp/cl/dl; cd -; echo "7 back pwd=$(pwd)"
pwd extra args; echo "8 st=$?"
pwd -x; echo "9 st=$?"
cd -L /tmp/cl/dl/sub; echo "10 $(pwd)"
cd /tmp; rm -rf cl clo
echo end
