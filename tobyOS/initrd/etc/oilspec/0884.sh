set -- -c foo -h
getopts 'hc:' opt
echo status=$? opt=$opt OPTARG=$OPTARG
getopts 'hc:' opt
echo status=$? opt=$opt OPTARG=$OPTARG
