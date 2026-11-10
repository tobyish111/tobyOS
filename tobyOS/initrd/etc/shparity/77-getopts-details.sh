# POSIX getopts: the silent-mode (leading ':') contracts -- '?' with OPTARG
# set for an unknown option, ':' with OPTARG for a missing argument -- and
# where OPTIND lands after the loop. Loud-mode diagnostics go to stderr,
# which the gate does not compare; the variables are what is pinned.
parse() {
  OPTIND=1
  while getopts "ab:c" opt "$@"; do
    echo "opt=$opt arg=${OPTARG-unset}"
  done
  echo "optind=$OPTIND"
}
parse -a -b val -c pos
parse -x
parse -b
sparse() {
  OPTIND=1
  while getopts ":ab:" opt "$@"; do
    echo "s-opt=$opt arg=${OPTARG-unset}"
  done
  echo "s-optind=$OPTIND"
}
sparse -x
sparse -b
sparse -a -b two rest
echo end
