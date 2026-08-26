# Parameter expansion. The ${x#...} / ${x%...} family is how shell scripts do
# string manipulation without calling out to a program, so a shell that lacks
# it forces every script through sed -- and stops being a superset.
x=hello
y=
echo "${x}"
echo "${x}world"
echo "${#x}"
echo "${y:-default}"
echo "${y:=assigned}"
echo "y is now [${y}]"
echo "${unset_var:-fallback}"
echo "${unset_var}"
p=/a/b/c.txt
echo "${p#/a/}"
echo "${p##*/}"
echo "${p%.txt}"
echo "${p%%.*}"
echo "${p#*/}"
