# Pattern operators DISTRIBUTE over the elements of ${a[@]}.
a=(foo.txt bar.txt baz.log)
echo "strip=${a[@]%.txt}"
echo "pre=${a[@]#ba}"
echo "sub=${a[@]/./_}"
echo "up=${a[@]^^}"
b=(one two)
for x in "${b[@]/o/0}"; do echo "w:[$x]"; done
echo end
