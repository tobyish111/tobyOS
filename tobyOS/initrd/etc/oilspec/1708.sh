touch {1,10}.txt
mkdir -p foo
touch foo/{2,20}.txt
GLOBIGNORE=?.txt
echo *.* foo/*.*
