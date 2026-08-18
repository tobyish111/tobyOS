set -o allexport
VAR1=value1
set +o allexport
VAR2=value2
printenv.py VAR1 VAR2
