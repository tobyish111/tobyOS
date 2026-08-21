# set -m / set -o monitor: job control's OPTION. POSIX requires the option
# to exist; tsh rejected the name outright. This case pins acceptance, the
# short flag, $- reporting and the set +o round-trip. The deeper monitor
# semantics (per-job process groups, status lines before the prompt) are
# interactive-only and tracked separately -- $- is checked by PRESENCE
# (case pattern), never by full string, because the two shells legitimately
# carry different default flags.
set -o monitor; echo "1=$?"
case $- in *m*) echo "2=m-on";; *) echo "2=m-off";; esac
set +o monitor; echo "3=$?"
case $- in *m*) echo "4=m-on";; *) echo "4=m-off";; esac
set -m; case $- in *m*) echo "5=m-on";; *) echo "5=m-off";; esac
set +m; case $- in *m*) echo "6=m-on";; *) echo "6=m-off";; esac
( set -m; set +o | grep monitor )
( set +o | grep monitor )
( set -o monitor; set -o | grep monitor )
( set -m; true & wait; echo "9=$?" )
echo end
