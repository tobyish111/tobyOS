# POSIX 2.8.2 -- exit status of every construct.
# A pipeline's status is its LAST command's; `!` inverts to exactly 0 or 1;
# a compound's status is that of the last command it ran; an empty loop is 0.
true;  echo "1=$?"
false; echo "2=$?"
! true;  echo "3=$?"
! false; echo "4=$?"

if true; then true; fi;  echo "5=$?"
if true; then false; fi; echo "6=$?"
if false; then true; fi; echo "7=$?"
if false; then :; else false; fi; echo "8=$?"

for i in 1 2; do true; done;  echo "9=$?"
for i in 1 2; do false; done; echo "10=$?"
for i in; do false; done;     echo "11=$?"

while false; do true; done; echo "12=$?"
until true; do false; done; echo "13=$?"

case x in x) true ;; esac;  echo "14=$?"
case x in x) false ;; esac; echo "15=$?"
case x in y) false ;; esac; echo "16=$?"

{ true; };  echo "17=$?"
{ false; }; echo "18=$?"
(true);  echo "19=$?"
(false); echo "20=$?"
(exit 7); echo "21=$?"

f() { return 3; }; f; echo "22=$?"
g() { false; };    g; echo "23=$?"
h() { :; };        h; echo "24=$?"

true | false; echo "25=$?"
false | true; echo "26=$?"
:; echo "27=$?"
echo end
