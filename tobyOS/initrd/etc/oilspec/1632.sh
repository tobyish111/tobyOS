# not supported in OSH due to GlobToERE() strategy for positional info

shopt -s extglob
x=foo.py
echo ${x//@(?.py)/Z}
