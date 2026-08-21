# POSIX ulimit: the operand forms. This kernel keeps no resource limits, so
# every READBACK is `unlimited` -- the real bash in the initrd reads its
# limits from the same kernel and prints the same thing. Statuses and the
# accepted argument forms are what the case pins.
ulimit
echo "1=$?"
ulimit -f
echo "2=$?"
ulimit -f unlimited
echo "3=$?"
ulimit -f 100
echo "4=$?"
ulimit -f
ulimit -z
echo "6=$?"
ulimit -f notanumber
echo "7=$?"
echo end
