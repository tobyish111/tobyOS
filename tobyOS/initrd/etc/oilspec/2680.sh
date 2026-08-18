# frontend/lexer_def.py has rules for this

tab=$(python2 -c 'print "argv.py -\t-"')
cr=$(python2 -c 'print "argv.py -\r-"')
vert=$(python2 -c 'print "argv.py -\v-"')
ff=$(python2 -c 'print "argv.py -\f-"')

$SH -c "$tab"
$SH -c "$cr"
$SH -c "$vert"
$SH -c "$ff"
