# declare -f with an attribute letter sets it on the function and prints
# nothing; +r of a readonly function is "NAME: readonly function"; -a -A -i
# -n with -f are "invalid option", status 1; and NAME=VALUE is "cannot use
# `-f' to make functions" (finding 413).
exec 2>&1
g() { echo g; }
declare -fr g; echo "rc=$?"; declare -f +r g; echo "rc=$?"; declare -ft g; echo "rc=$?"
g() { :; }; echo "rc=$?"
h() { echo h; }
declare -f -a h; echo "rc=$?"; declare -f -i b c; echo "rc=$?"; declare -f -A h; echo "rc=$?"
declare -f -n h; echo "rc=$?"; declare -f -l h; echo "rc=$?"
declare -f k='() { echo k; }'; echo "rc=$?"; declare -f x=1 h; echo "rc=$?"
