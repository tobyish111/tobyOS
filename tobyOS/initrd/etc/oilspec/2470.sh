# This is a PARSING divergence.  In OSH we match [], rather than using POSIX
# rules!

pat='[^]]'
s='ab^cd^'
echo ${s//$pat/z}
