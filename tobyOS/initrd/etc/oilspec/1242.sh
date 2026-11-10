trap 'echo IN TRAP; echo $stdout' EXIT 
stdout=FOO
exit 42
