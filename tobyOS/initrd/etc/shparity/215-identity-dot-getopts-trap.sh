# UID and EUID (readonly), HOSTNAME and OSTYPE are set; `.` skips a directory
# on PATH and takes --; getopts parses even when NAME is bad (and fails
# with 1), and past the end OPTIND is $#+1; `trap SIG` alone resets SIG.
# tsh had none of the variables, sourced the directory, refused -- and
# the bad NAME before parsing, rewound OPTIND to 1 and printed usage.
exec 2>&1
cd "${TMPDIR:-/tmp}" || exit 1
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
