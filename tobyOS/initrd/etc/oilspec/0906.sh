# Leading : in optspec enables silent mode: OPTARG=option char, no error msg
set -- -Z
getopts ':a:' opt 2>&1
echo "status=$? opt=$opt OPTARG=$OPTARG"
