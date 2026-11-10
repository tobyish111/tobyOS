# Notes on whitespace:
# - 1 and == need space seprating them, but ! and ( don't.
# - [[ needs whitesapce after it, but ]] doesn't need whitespace before it!
[[ ''||! (1 == 2)&&(2 == 2)]] && echo true

# NOTE on the two cases below.  We're comparing
#   (a || b) && c   vs.   a || (b && c)
#
# a = true, b = false, c = false is an example where they are different.
# && and || have precedence inside
