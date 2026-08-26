# Bug fix due to '' being falsey in Python
compgen -W '' -- foo
echo status=$?
