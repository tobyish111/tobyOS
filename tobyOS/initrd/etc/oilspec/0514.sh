f0() { return 3;          echo unexpected; return 0; }
f1() { \return 3;         echo unexpected; return 0; }
f0; echo "status=$?"
f1; echo "status=$?"
