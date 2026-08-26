# This tickles an infinite loop bug in our version of mksh!  TODO: upgrade the
# version and enable this
case $SH in mksh) return ;; esac

cd $TMP
mkdir -p dir
read x < ./dir
echo status=$?

# OK mksh stdout: status=2
