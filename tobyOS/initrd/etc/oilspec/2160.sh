# can't have nul byte

# This pattern has literal characters
pat=$'^[\x01\x02]+$'

[[ $'\x01\x02\x01' =~ $pat ]]; echo status=$?
[[ $'a\x01' =~ $pat ]]; echo status=$?

# NOTE: There doesn't appear to be any way to escape these!
pat2='^[\x01\x02]+$'
