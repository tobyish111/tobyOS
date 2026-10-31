# A pattern component with no glob character in it is a name: */. is each
# directory's `.`, a directory that can be listed but not searched
# included -- tsh matched `.` against a listing that has none (finding 462).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
mkdir -m a=x searchable; mkdir -m a=r readable; mkdir -p plain/sub; touch plain/f
echo */.; p='*/\.'; echo $p; echo */'.'; echo */..; echo p*/sub; echo */f
cd /; chmod -R u+rwx "$d"; rm -rf "$d"
