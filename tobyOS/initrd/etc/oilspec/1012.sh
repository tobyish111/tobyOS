case $SH in mksh) echo 'weird bug'; exit ;; esac

echo too large
too_large=$(python2 -c 'print("\xF4\x91\x84\x91")')
printf '%x\n' \'$too_large
printf '%u\n' \'$too_large
printf '%o\n' \'$too_large
echo




# osh rejects code points that are too large for a DIFFERENT reason
