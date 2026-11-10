# A script with no #! line is run as a fresh shell but for globskipdots,
# which it keeps from the shell that runs it: after `shopt -u globskipdots`
# its `echo .*` lists . and .. too. tsh's script took the default
# (finding 579).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit 1
mkdir g; touch g/.a g/b
printf 'cd g; echo .*\nshopt globskipdots extglob\n' > s; chmod +x s
shopt -s extglob
shopt -u globskipdots; ./s
shopt -s globskipdots; ./s
cd /; rm -rf "$d"
