# Note: "busybox sh" behaves strangely.
y=a=123 n=a=321
echo $((1?(y):(n))):$((a))
echo $((0?(y):(n))):$((a))
