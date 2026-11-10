# set -o ignoreeof sets IGNOREEOF to 10, over any value, and set +o unsets
# it; the option is on while IGNOREEOF is set (finding 382). tsh's option
# and variable did not know each other.
exec 2>&1
unset IGNOREEOF; set -o ignoreeof; echo "[$IGNOREEOF]"; set +o ignoreeof; echo "[${IGNOREEOF-unset}]"
IGNOREEOF=0; shopt -o ignoreeof; set -o | grep ignoreeof; unset IGNOREEOF; shopt -o ignoreeof
IGNOREEOF=5; set -o ignoreeof; echo "[$IGNOREEOF]"; set +o ignoreeof
