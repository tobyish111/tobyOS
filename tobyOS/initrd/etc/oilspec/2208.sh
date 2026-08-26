$SH -c 'echo $-' | grep i || echo FALSE
$SH -i -c 'echo $-' | grep -q i && echo TRUE
