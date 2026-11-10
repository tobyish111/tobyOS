# Normal mode returns '?', OPTARG is empty
set -- -a
getopts 'a:' opt 2>/dev/null
echo "status=$? opt=$opt OPTARG=$OPTARG"
