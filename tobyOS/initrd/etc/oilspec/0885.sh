set -u
getopts 'ab' name '-a'
echo name=$name
echo OPTARG=$OPTARG
