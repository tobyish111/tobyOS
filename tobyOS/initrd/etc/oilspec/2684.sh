# dash and zsh don't have echo -e
tab=$(python2 -c 'print "-\t-"')
cr=$(python2 -c 'print "-\r-"')
vert=$(python2 -c 'print "-\v-"')
ff=$(python2 -c 'print "-\f-"')

$SH -c 'argv.py $1' dummy0 "$tab"
$SH -c 'argv.py $1' dummy0 "$cr"
$SH -c 'argv.py $1' dummy0 "$vert"
$SH -c 'argv.py $1' dummy0 "$ff"


# No word splitting in zsh
