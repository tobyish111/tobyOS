mkdir {dir1,dir2}
touch dir1/{a.txt,ignore.txt}
touch dir2/{a.txt,ignore.txt}
GLOBIGNORE=*/ignore*
echo */*
