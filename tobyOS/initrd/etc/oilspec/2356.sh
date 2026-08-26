HOME=/home/bar
x=foo:~
echo $x
echo "$x"  # quotes don't matter, the expansion happens on assignment?
x='foo:~'
echo $x

x=foo:~,  # comma ruins it, must be /
echo $x

x=~:foo
echo $x

# no tilde expansion here
echo foo:~
