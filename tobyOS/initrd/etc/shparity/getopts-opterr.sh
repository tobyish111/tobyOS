# OPTERR decides whether getopts prints its own messages: quiet when its
# leading number reads 0, loud when it is unset or empty; read when getopts
# runs, so a local OPTERR and a prefix count (finding 316). tsh never
# looked at it.
OPTERR=0; while getopts ab o -a -c -b; do echo "o=$o ${OPTARG-unset}"; done 2>&1; echo "rc=$? $OPTIND"
OPTIND=1; getopts b: o -b 2>&1; echo "o=$o"
for v in 00 '' 2 abc -0 ' 0' 1abc ' 1' +1 -1 0x1 08 x1 ' ' '1 '; do
    OPTERR=$v; OPTIND=1; getopts a o -z 2>&1; echo "[$v] o=$o"
done
unset OPTERR; OPTIND=1; getopts a o -z 2>&1; echo "unset o=$o"
f() { local OPTERR=0; OPTIND=1; getopts a o -z; echo "f o=$o"; }; f 2>&1
OPTIND=1; getopts a o -z 2>&1; echo "after o=$o"
OPTIND=1; OPTERR=0 getopts a o -z 2>&1; echo "prefix o=$o"
