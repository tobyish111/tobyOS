# gzip/gunzip/zcat exercise, run by /bin/tsh inside the guest.
#
# Two fixtures ship in the image and matter for different reasons:
#
#   etc/gnumade.gz    made by REAL GNU gzip -9. Its first block is DYNAMIC
#                     Huffman (BTYPE=2), which our own encoder never emits,
#                     so this is the only thing here that proves the
#                     inflater handles the half of RFC 1951 we do not write.
#   etc/gnubadcrc.gz  the same file with one bit flipped in the stored
#                     CRC-32. The deflate stream is still perfectly valid,
#                     so puff inflates it happily -- ONLY a real trailer
#                     check rejects it. If -t ever passes this, the CRC
#                     verification has silently stopped happening.

cd "${WORKDIR:-/tmp}"

printf 'hello gzip\n' > plain.txt
# Big enough to cross the 32 KiB match window, so distances wrap.
i=0
: > big.txt
while [ $i -lt 900 ]; do
  echo "row $i the quick brown fox jumps over the lazy dog padding padding" >> big.txt
  i=$((i + 1))
done
: > empty.txt

echo @@gz_pipe
printf 'round trip me\n' | gzip | gunzip

echo @@gz_file
cp plain.txt f1.txt
gzip f1.txt
echo "gz exists: $([ -f f1.txt.gz ] && echo yes || echo no)"
echo "orig gone: $([ -f f1.txt ] && echo no || echo yes)"
gunzip f1.txt.gz
echo "restored: $(cat f1.txt)"

echo @@gz_keep
cp plain.txt f2.txt
gzip -k f2.txt
echo "orig kept: $([ -f f2.txt ] && echo yes || echo no)"

echo @@gz_stdout
gzip -c plain.txt | gunzip

echo @@gz_zcat
gzip -c plain.txt > z1.gz
zcat z1.gz

echo @@gz_levels
gzip -1 -c big.txt | gunzip | wc -l
gzip -9 -c big.txt | gunzip | wc -l

echo @@gz_big
gzip -c big.txt > big.gz
gunzip -c big.gz > big.out
echo "same size: $([ "$(wc -c < big.txt)" = "$(wc -c < big.out)" ] && echo yes || echo no)"
echo "first: $(head -1 big.out)"
echo "last: $(tail -1 big.out)"

echo @@gz_empty
gzip -c empty.txt > e.gz
echo "empty bytes back: $(gunzip -c e.gz | wc -c)"

echo @@gz_test_ok
gzip -t z1.gz; echo rc=$?

echo @@gz_gnu
# The dynamic-Huffman fixture. Its content is checked by hash, not by eye.
zcat /etc/gnumade.gz > gnu.out
echo "lines: $(wc -l < gnu.out)"
echo "bytes: $(wc -c < gnu.out)"
echo "first: $(head -1 gnu.out)"
echo "last: $(tail -1 gnu.out)"
sha256sum gnu.out | cut -d' ' -f1

echo @@gz_gnu_test
gzip -t /etc/gnumade.gz; echo rc=$?

echo @@gz_badcrc
gzip -t /etc/gnubadcrc.gz 2>&1; echo rc=$?

echo @@gz_notgz
gunzip -c plain.txt 2>&1; echo rc=$?

echo @@gz_list
gzip -l z1.gz | tail -1 | wc -w

echo @@gz_END
