tobyOS -- milestone 4 demo initrd
=================================

This is a plain text file shipped inside the boot ramfs.
You are reading it through the kernel VFS layer:

    cat /readme.txt
        |
        +--> shell  cmd_cat
                |
                +--> vfs_open("/readme.txt") + vfs_read()
                        |
                        +--> ramfs_ops.open / .read
                                |
                                +--> walk USTAR header table built
                                     at boot from /initrd.tar
                                     (loaded by Limine as a module)

The kernel does NO copy of the file: the ramfs serves bytes
directly out of the original tar payload via memcpy into your
read buffer. /bin/hello is loaded the same way -- the ELF
loader pulls the bytes from VFS, maps PT_LOAD segments into
the user half, and switches to ring 3.
