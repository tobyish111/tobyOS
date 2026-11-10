f() { return 300; }; f; echo r=$?
f() { return -1; }; f; echo r=$?
g() { return abc; echo no; }; g; echo r=$?
h() { return -- 7; }; h; echo r=$?
(exit 256); echo e=$?
(exit " 3 "); echo e=$?
(exit 99999999999); echo e=$?
(exit -- 4); echo e=$?
(exit 0x10); echo e=$?
(exit abc; echo no); echo e=$?
exit 7 8
echo after-too-many st=$?
