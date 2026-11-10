# extglob is OFF.  Doesn't affect bash or mksh!
[[ cc == @(cc) ]] 
echo status=$?
[[ cc == '@(cc)' ]]
echo status=$?

shopt -s extglob

[[ cc == @(cc) ]]
echo status=$?
[[ cc == '@(cc)' ]]
echo status=$?
