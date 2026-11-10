exec 2>&1
busy() { e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
x=set
sleep 1 & kill -9 $!; busy; echo "A-before"; echo ${x:-$(echo n)}; echo "A-after"
sleep 1 & kill -9 $!; busy; echo "B-before"; echo ${x:+$(echo p)}; echo "B-after"
sleep 1 & kill -9 $!; busy; echo "C-before"; echo ${u-$(echo q)}; echo "C-after"
sleep 1 & kill -9 $!; busy; echo "D-before"; echo ${x#$(echo s)}; echo "D-after"
sleep 1 & kill -9 $!; busy; echo "E-before"; echo "${x:-`echo b`}"; echo "E-after"
sleep 1 & kill -9 $!; busy; echo "F-before"; : ${x:=$(echo f)}; echo "F-after"
sleep 1 & kill -9 $!; busy; echo "G-before"; echo ${x:-<(true)}; echo "G-after"
echo end
