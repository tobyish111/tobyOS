touch one.md one.txt
mkdir -p foo
touch foo/{two.md,two.txt}
GLOBIGNORE=*.txt
echo *.* foo/*.*
