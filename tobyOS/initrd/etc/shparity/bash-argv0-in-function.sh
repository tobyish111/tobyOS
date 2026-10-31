# $0 set through BASH_ARGV0 in a function stays set after it returns --
# a function does not own $0 -- tsh put the caller's back (finding 516).
exec 2>&1
BASH_ARGV0=hello
echo "[$0]"
setarg0() { BASH_ARGV0="$1"; echo "in [$0]"; }
setarg0 arg0
echo "[$0] [$BASH_ARGV0]"
f() { :; }; f
echo "[$0]"
