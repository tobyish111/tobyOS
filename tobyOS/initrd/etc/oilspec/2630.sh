echo "${var-a\nb}"
echo "${var:-c\nd}"
var=val
echo "${var:+e\nf}"
