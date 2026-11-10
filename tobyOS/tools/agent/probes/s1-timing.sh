# s1-timing.sh -- run under bash: how long does each shell take to return
# when it leaves a background job running?
ms() { echo $(( (${2/./} - ${1/./}) / 1000 )); }
s=$EPOCHREALTIME; /bin/tsh -c 'sleep 3 &'; e=$EPOCHREALTIME; echo "tsh -c 'sleep 3 &': $(ms $s $e) ms"
s=$EPOCHREALTIME; /bin/tsh -c 'sleep 3 & true'; e=$EPOCHREALTIME; echo "tsh -c 'sleep 3 & true': $(ms $s $e) ms"
printf 'sleep 3 &\necho started\n' > /tmp/s1.sh
s=$EPOCHREALTIME; /bin/tsh /tmp/s1.sh; e=$EPOCHREALTIME; echo "tsh script: $(ms $s $e) ms"
s=$EPOCHREALTIME; bash -c 'sleep 3 &'; e=$EPOCHREALTIME; echo "bash -c 'sleep 3 &': $(ms $s $e) ms"
s=$EPOCHREALTIME; bash /tmp/s1.sh; e=$EPOCHREALTIME; echo "bash script: $(ms $s $e) ms"
rm -f /tmp/s1.sh
