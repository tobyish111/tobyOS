# ext2/3/4 tool transcript for logs/e2fstools.sh.
#
# The two extra drives carry HAND-BUILT superblocks with known values, so
# every number these tools print can be checked against what was written --
# a real mkfs is not available here (userland block access is read-only by
# design), and a filesystem whose contents I do not know would only prove
# the tools did not crash.
echo @@e2_lsblk
lsblk
echo @@e2_find
# Find whichever device carries each crafted label. Device naming depends on
# how QEMU enumerates the drives, so discover it rather than hardcode it.
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then echo "good=$d"; fi
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYBAD;  then echo "bad=$d";  fi
done
echo @@e2_dumpe2fs
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  dumpe2fs -h $d 2>/dev/null | grep -E 'volume name|Block count|Inode count|Block size|state' && break
done
echo @@e2_e2fsck_clean
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then e2fsck -n $d; echo rc=$?; break; fi
done
echo @@e2_e2fsck_bad
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYBAD; then e2fsck -n $d; echo rc=$?; break; fi
done
echo @@e2_e2fsck_refuses_repair
e2fsck -y /dev/null 2>&1; echo rc=$?
echo @@e2_debugfs
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then debugfs -R stats $d; break; fi
done
echo @@e2_debugfs_refuses_write
debugfs -w /dev/null 2>&1; echo rc=$?
echo @@e2_badblocks
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then badblocks -v $d 64 0 2>&1; echo rc=$?; break; fi
done
echo @@e2_badblocks_refuses_write
badblocks -w /dev/null 2>&1; echo rc=$?
echo @@e2_resize_p
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then resize2fs -P $d; break; fi
done
echo @@e2_resize_write
for d in $(lsblk | tail -n +2 | cut -d' ' -f1); do
  # No pipeline: `echo rc=$?` after one reports the LAST command's status,
  # not resize2fs's, and merging stderr into a pipe lost the message too.
  if dumpe2fs -h $d 2>/dev/null | grep -q TOBYGOOD; then
    MSG=$(resize2fs $d 2>&1)
    echo rc=$?
    echo "$MSG"
    break
  fi
done
echo @@e2_notext4
dumpe2fs -h nosuchdev 2>&1; echo rc=$?
echo @@e2_END
