shopt -s extglob
mkdir -p opts
cd opts

touch -- foo bar -dash
echo @(*)

shopt --set no_dash_glob
echo @(*)
