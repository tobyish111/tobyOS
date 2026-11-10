# dqansi.sh: $'...' inside double quotes, in a ${x-WORD}, in a pattern and
# nested in a pattern's default word (finding 112's last line).
exec 2>&1
v='a*b'
echo "1 [${v#$'a*'}] [${v#$'a'}] [${v#$'\x61'}]"
echo "2 [${v#${u:-$'a*'}}] [${v#${u:-$'\x61*'}}] [${v#${u:-$'a'}}]"
echo "3 [${u:-$'a\x41'}] [${u:-$'a*'}]"
echo "4" ${v#${u:-$'a*'}} ${v#$'a*'}
echo "5 [${v/${u:-$'*'}/X}] [${v/$'*'/X}]"
echo "6 [${v#"${u:-$'a*'}"}] [${v#${u:-"$'a*'"}}]"
x=$'\x61'; echo "7 [${v#${u:-$x}}]"
echo end
