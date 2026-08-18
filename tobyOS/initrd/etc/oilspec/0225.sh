IFS=x
set -- x y z
var="[$@]"
argv.py "$var"
