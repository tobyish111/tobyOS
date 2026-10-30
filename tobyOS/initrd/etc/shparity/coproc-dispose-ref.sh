# When a coprocess ends its NAME_PID goes, readonly or not, and NAME goes
# through a reference -- the reference stays -- or, readonly, says so (finding
# 336). tsh removed the reference itself, kept a readonly NAME_PID and said
# nothing.
{
( declare -n d=x; coproc d { :; }; wait; declare -p d; declare -p x )
( declare -r ROVAR=42; coproc ROVAR { :; }; wait; declare -p ROVAR )
( declare -r RO RO_PID; coproc RO { :; }; wait; declare -p RO; declare -p RO_PID )
} 2>&1 | sed 's/=([^)]*)/=(...)/'
