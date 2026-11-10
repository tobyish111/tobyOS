# OK in both bash and mksh
foo=abcdefg
echo _${foo:3:100}
echo $?
