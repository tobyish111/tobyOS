# vmstat / tload / ss / skill and the three that have no data source.
# Run by /bin/tsh in the guest.
#
# The point of the last group is that they must REFUSE and name what is
# missing. A tool that prints a table of zeros looks like a working tool
# reporting an idle machine, which is the failure mode worth guarding.

cd "${WORKDIR:-/tmp}"

echo @@pn_vmstat_header
vmstat 2>/dev/null | head -2

echo @@pn_vmstat_row
# The guest just PRINTS; the host does the arithmetic. Chaining
# cut/tr/sed in here to derive a number made the test itself the thing
# under suspicion when it failed.
vmstat 2>/dev/null | tail -1

echo @@pn_meminfo
grep MemFree /proc/meminfo

echo @@pn_vmstat_says_why
vmstat 2>&1 >/dev/null | head -1

echo @@pn_tload
tload | head -1

echo @@pn_ss_header
ss -t | head -1

echo @@pn_ss_listen
# sshd listens inside the image, so -l must find at least the header and
# should not error.
ss -lt > ss.txt 2>&1; echo rc=$?
echo "lines: $(wc -l < ss.txt)"

echo @@pn_skill
sleep 40 &
sleep 1
skill -TERM sleep; echo "skill rc=$?"
sleep 1
# ps is procps-style now: -C selects by command name, pid= drops the header.
# (The old grep matched the old /bin/ps's SERVICE listing, which never had a
# sleep in it, so this line passed whether or not skill had worked.)
echo "still running: $(ps -C sleep -o pid= 2>/dev/null | grep -c .)"

echo @@pn_skill_nomatch
skill -TERM nosuchprocessname; echo rc=$?

echo @@pn_slabtop
slabtop 2>&1; echo rc=$?

echo @@pn_bridge
bridge 2>&1; echo rc=$?

echo @@pn_tracepath
tracepath 1.2.3.4 2>&1; echo rc=$?

echo @@pn_snice
snice +1 sleep 2>&1; echo rc=$?

echo @@procnet_END
