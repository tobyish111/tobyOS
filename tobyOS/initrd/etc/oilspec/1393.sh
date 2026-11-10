# virtualenv's bin/activate uses this.
# This is weird!  Double quotes within `` is different than double quotes
# within $()!  All shells agree.
# I think this is related to the nested backticks case!
echo "x $(echo hi)"
echo "x $(echo "hi")"
echo "x $(echo \"hi\")"
echo "x `echo hi`"
echo "x `echo "hi"`"
echo "x `echo \"hi\"`"
