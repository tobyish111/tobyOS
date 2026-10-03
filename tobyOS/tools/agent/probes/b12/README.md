# Batch 12 probes

Each `pN.sh` (and `fn1.sh`, `cpk*.sh`) is one question put to the guest's
bash 5.2.15 and to /bin/tsh: the batch's commits cite them as "probe pN".
Run one against a booted guest (QEMU on port 2222 here; the EliteDesk by
default):

    ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root \
        bash tools/agent/probe3.sh logs/b12/pN tools/agent/probes/b12/pN.sh

which leaves `logs/b12/pN.bash` and `logs/b12/pN.tsh` and says IDENTICAL or
DIFFERENT. A probe prints values that are the same in both shells -- pids,
times and the shell's own name are left out or normalised where they could
show (p49 p66 p67 p74 p80 print a pid in a job report and differ there only;
p31 p77 print BASH/MACHTYPE and the environment order, which differ by
design).

The deterministic ones became shparity fixtures 220-248
(initrd/etc/shparity), one fixture per probe.
