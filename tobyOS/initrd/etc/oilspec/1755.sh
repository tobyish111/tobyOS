<<EOF tac
1
2
3
EOF

  # NOTE that you can have redirection AFTER the here doc thing.  And you don't
  # need a space!  Those are operators.
  #
  # POSIX doesn't seem to have this?  They have io_file, which is for
  # filenames, and io_here, which is here doc.  But about 1>&2 syntax?  Geez.
