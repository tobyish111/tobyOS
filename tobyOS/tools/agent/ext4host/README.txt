ext4host -- src/ext4.c on the host (finding 271).

ext4host.c includes the driver whole, with stub headers (stub/tobyos/) for
the kernel heap, printk and klibc, and a block device that is an image
file; it drives the driver's vfs_ops the way the VFS does, from a command
language on stdin (see the comment at its top). Built in WSL:

  gcc -O1 -g -fsanitize=address,undefined -Itools/agent/ext4host/stub \
      -Iinclude tools/agent/ext4host/ext4host.c -o /tmp/ext4host
  /tmp/ext4host IMAGE [ro] < SCRIPT

Besides the operations: `manifest` lists every name as the driver reads
it, and `verify` recomputes every checksum on the volume (and the htree
hash of every name) with the driver's own functions and compares each with
what is stored -- run on a volume Linux made, it checks the formulas.

What the driver leaves is judged by Linux's tools: e2fsck -fn, a Linux
mount, and the scripts in logs/b34 (README there: ext4*.sh, ext4stress.py).
Run with ASAN_OPTIONS=detect_leaks=0 when the script does not end in
umount (the mount's memory is the driver's to free).
