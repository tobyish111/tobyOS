# declare -in NAME=VALUE makes nothing and says nothing, status 1: the
# value is a number before it is a name (finding 342). tsh made it.
{
( declare -ai c=(1); declare -in d="c[0]"; echo "rc=$?"; declare -p d; d+=1; declare -p d )
( declare -ai c=(1); declare -n e='c[0]'; declare -ni e; echo "rc=$?"; declare -p e )
} 2>&1
