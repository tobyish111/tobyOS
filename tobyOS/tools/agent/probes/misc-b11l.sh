s='foo()'; case $s in *\(\)) echo match;; esac
case abc in \*) echo star;; *) echo other;; esac
case 'a*' in a\*) echo lit;; esac
echo "$UID" | grep -q '^[0-9][0-9]*$' && echo uid-ok; echo "$EUID" | grep -q '^[0-9]' && echo euid-ok
UID=5; echo st=$?
test -n "$OSTYPE" && echo ostype-set
test "$HOSTNAME" = "$(hostname)" && echo host-ok
mkdir -p _d/dir/hello.sh _d/sh; printf 'echo hi\n' > _d/sh/hello.sh
PATH="$PWD/_d/dir:$PWD/_d/sh:$PATH" . hello.sh; echo st=$?
printf 'echo foo\n' > _f.sh; . -- _f.sh
set -- -c foo -h; getopts 'hc:' opt-; echo "st=$? opt=$opt OPTARG=$OPTARG OPTIND=$OPTIND"
OPTIND=1; set -- -a; while getopts a o; do :; done; set -- -f; while getopts f: o; do echo _; done; echo OPTIND=$OPTIND
trap 'echo E' EXIT; trap EXIT; trap -p
rm -rf _d _f.sh
