# hm bash also ignores -r when -n is set

x=XX
typeset -n -r ref=x

echo ref=$ref

# it feels like I shouldn't be able to mutate this?
ref=XXXX
echo ref=$ref

x=X
echo x=$x
