t0=$EPOCHREALTIME
for ((i=0;i<200;i++)); do true; true; done
t1=$EPOCHREALTIME
for ((i=0;i<200;i++)); do true | true; done
t2=$EPOCHREALTIME
for ((i=0;i<200;i++)); do x=$(echo a); done
t3=$EPOCHREALTIME
for ((i=0;i<200;i++)); do ( true ); done
t4=$EPOCHREALTIME
for ((i=0;i<200;i++)); do /bin/true; done
t5=$EPOCHREALTIME
echo "plain   $(( (${t1/./} - ${t0/./}) / 200 )) us"
echo "pipe    $(( (${t2/./} - ${t1/./}) / 200 )) us"
echo "comsub  $(( (${t3/./} - ${t2/./}) / 200 )) us"
echo "subsh   $(( (${t4/./} - ${t3/./}) / 200 )) us"
echo "extern  $(( (${t5/./} - ${t4/./}) / 200 )) us"
