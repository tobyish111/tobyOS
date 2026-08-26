# XCU 2.7.2: noclobber refuses `>` onto an existing file, but `>|` FORCES
# it and `>>` is never blocked. All in subshells so -C cannot leak.
echo first > nc94.txt
( set -C; echo again > nc94.txt ) 2>/dev/null
echo "1=$?"
cat nc94.txt
( set -C; echo forced >| nc94.txt; echo "2=$?" )
cat nc94.txt
( set -C; echo appended >> nc94.txt; echo "3=$?" )
cat nc94.txt
( set -C; echo fresh > brand-new94.txt; echo "4=$?" )
cat brand-new94.txt
echo end
