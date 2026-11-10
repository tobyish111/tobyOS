# Does a `.` that cannot read its file still run the RETURN trap? (Oils 1193
# sources a helper that is not in the guest.) One line per failing form.
trap 'echo "  RETURN trap ran"' RETURN
echo "missing with a slash:"; . ./nosuch_drt.sh; echo "  status=$?"
echo "missing, no slash:"; . nosuch_drt.sh; echo "  status=$?"
echo "a directory:"; . /tmp; echo "  status=$?"
echo "no operand:"; . ; echo "  status=$?"
trap - RETURN
echo done
