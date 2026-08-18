# bash doesn't detect as many errors because it lacks static parsing.
compgen -W '${'
echo status=$?
complete -W '${' foo
echo status=$?
