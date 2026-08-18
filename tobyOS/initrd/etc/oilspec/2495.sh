$SH -c 'array=(1 2 3); argv.py ${array[@]:}'
$SH -c 'array=(1 2 3); argv.py space ${array[@]: }'

$SH -c 's=123; argv.py ${s:}'
$SH -c 's=123; argv.py space ${s: }'
