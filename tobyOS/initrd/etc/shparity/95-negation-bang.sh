# XCU 2.9.2: `!` negates a PIPELINE's status. Double negation, negation in
# conditionals, and interaction with && / ||.
! false
echo "1=$?"
! true
echo "2=$?"
! ! true
echo "3=$?"
! true && echo and-ran || echo or-ran
if ! false; then echo in-if; fi
! false | false
echo "6=$?"
echo end
