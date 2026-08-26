# POSIX newgrp + id, over the /etc/passwd and /etc/group this repo ships.
# BOTH shells run the same two external binaries, so parity alone cannot
# catch a same-for-both breakage: the expected column below is part of the
# case and must be re-checked against the gate log whenever these programs
# change. Its total is 90 BYTES -- the PASS line's byte count is the check.
#
# KERNEL GAP dodged by design: fd offsets are not shared across the
# Linux->native exec seam (documented in handoff-shell-posix.md), so a
# grandchild writing the case's inherited stdout can be silently
# overwritten by the outer shell's next write. Inner shells therefore
# write their observations to FILES the outer shell cats afterwards.
#
# Expected: /bin/newgrp / 0 / root / "0 10 20" / "root wheel crew" /
# 1=0 / 2=0 / 3=0 / 4=1 / 5=42 / g1:20 / g2:wheel / g3:0 / 0 / end
SHELL=/bin/bash
export SHELL
command -v newgrp
id -g
id -gn
id -G root
id -Gn root
printf 'id -g > g1\nexit\n' | newgrp crew
echo "1=$?"
printf 'id -gn > g2\nexit\n' | newgrp wheel
echo "2=$?"
printf 'id -g > g3\nexit\n' | newgrp
echo "3=$?"
newgrp nosuchgroup
echo "4=$?"
printf 'exit 42\n' | newgrp crew
echo "5=$?"
echo "g1:$(cat g1)"
echo "g2:$(cat g2)"
echo "g3:$(cat g3)"
id -g
echo end
