f() {
	local x=local
	readonly x
	echo $x
	eval 'x=bar'  # Wrap in eval so it's not fatal
	echo status=$?
}
x=global
f
echo $x

# mksh aborts the function, weird
