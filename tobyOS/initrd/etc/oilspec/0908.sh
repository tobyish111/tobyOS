# Normal mode: OPTARG is empty, prints error message
set -- -Z
getopts 'a:' opt 2>/dev/null
echo "status=$? opt=$opt OPTARG=$OPTARG"
