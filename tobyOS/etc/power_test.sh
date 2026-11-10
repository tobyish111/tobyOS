# shutdown / poweroff / halt / reboot -- the SAFE cases.
#
# Nothing here actually powers the machine off: -k does everything except
# pull the trigger. The real path is proven separately by logs/power.sh,
# which boots an image that calls poweroff for real and checks that QEMU
# exited on its own.

cd "${WORKDIR:-/tmp}"

echo @@pw_dry_off
shutdown -k now; echo rc=$?

echo @@pw_dry_reboot
shutdown -r -k now; echo rc=$?

echo @@pw_dry_halt
shutdown -H -k now; echo rc=$?

echo @@pw_no_time
shutdown 2>&1; echo rc=$?

echo @@pw_bad_time
shutdown -k tomorrow 2>&1; echo rc=$?

echo @@pw_broadcast
shutdown -k now "maintenance window" 2>&1 | head -4

echo @@pw_poweroff_dry
poweroff -k; echo rc=$?

echo @@pw_reboot_dry
reboot -k; echo rc=$?

echo @@pw_halt_dry
halt -k; echo rc=$?

echo @@power_END
