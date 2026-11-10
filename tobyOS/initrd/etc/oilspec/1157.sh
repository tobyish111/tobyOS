set -a
FOO=exported
set +a
BAR=not_exported
printenv.py FOO BAR
