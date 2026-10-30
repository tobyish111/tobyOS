# getopts with fewer than two words prints bash's usage line, and getopts
# takes no options: `--` ends them, a lone `-` is the optstring, any other
# word that starts with `-` is "-X: invalid option" and the usage line,
# status 2 (finding 317). tsh had a usage line of its own and took -a for
# the optstring.
{
getopts; echo "rc=$?"; getopts ab; echo "rc=$?"
getopts -a ab o; echo "rc=$?"; getopts -ab o x; echo "rc=$?"
getopts -- ab o -a; echo "rc=$? o=$o"
OPTIND=1; getopts - o -x; echo "rc=$? o=$o"; OPTIND=1; getopts --x ab o; echo "rc=$?"
getopts -x- ab o; echo "rc=$?"; getopts --- ab o; echo "rc=$?"; getopts -1 ab o; echo "rc=$?"
getopts -- ; echo "rc=$?"; getopts -- ab; echo "rc=$?"
f() { getopts; }; f; echo "rc=$?"
} 2>&1
