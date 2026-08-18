set -- -h -c foo
getopts 'hc:' opt
echo status=$? opt=$opt
getopts 'hc:' opt
echo status=$? opt=$opt
getopts 'hc:' opt
echo status=$? opt=$opt
