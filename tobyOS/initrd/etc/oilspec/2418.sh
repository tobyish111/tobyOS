# Hm this works in demo/survey-case-fold.sh
# Is this a bash 4.4 thing?

#export LANG='tr_TR.UTF-8'
#echo $LANG

x='i'

echo u ${x^}
echo U ${x^^}

echo l ${x,}
echo L ${x,,}
