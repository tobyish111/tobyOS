output=$(times)
echo "$output" | while read line
do
	echo "$line" | egrep -q '[0-9]+m[0-9]+.[0-9]+s [0-9]+m[0-9]+.[0-9]+s' && echo "pass"
done

echo "$output" | wc -l
