shopt -s expand_aliases

alias ll='ls -l'
ll '1
  2
  3'
echo status=$?
