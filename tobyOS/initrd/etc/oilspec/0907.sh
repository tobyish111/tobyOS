# Silent mode returns ':' and sets OPTARG to option char
set -- -a
getopts ':a:' opt 2>&1
echo "status=$? opt=$opt OPTARG=$OPTARG"
