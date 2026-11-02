# A read timeout shorter than a millisecond is still a wait that runs out:
# `read -t 0.00001` with nothing to read is status 142, where tsh kept
# three digits of it and looked at the input as -t 0 does; and the time
# runs from before the first wait, so with the input already there such a
# read still runs out first and reads nothing (finding 585).
sleep 1 | { read -t 0.00001 v; echo "rc=$? [${v-unset}]"; }
sleep 1 | { read -t 0.0004 v; echo "rc=$? [${v-unset}]"; }
read -t 0.00001 v <<< abc; echo "rc=$? [$v]"
sleep 1 | { read -t 0; echo "rc=$?"; }
read -t 0 <<< abc; echo "rc=$?"
read -t 2.5 v <<< abc; echo "rc=$? [$v]"
