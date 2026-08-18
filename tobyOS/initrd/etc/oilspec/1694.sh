# osh does this "correctly" because it defers to libc!
touch $TMP/_G
cd $TMP
echo _[^\[z] _[^\]z]  # the right way to do it
echo _[^[z] _[^]z]    # also accepted
