# `!` with nothing after it negates a null command (bash's BANG
# list_terminator): `!` is 1, `! !` is 0, and neither runs a command named !
exec 2>&1
!
echo "bang: $?"
! !
echo "bang bang: $?"
! ! true
echo "bang bang true: $?"
true; !; echo "after true, bang: $?"
false; ! ; echo "after false, bang semi: $?"
eval '! !'; echo "eval bang bang: $?"
if !; then echo then; else echo else; fi
