x=XX
y=YY
typeset -n ref=x
echo ref=$ref
echo x=$x
echo y=$y

echo ----
typeset -n ref=y
echo ref=$ref
echo x=$x
echo y=$y
echo ----
ref=z
echo ref=$ref
echo x=$x
echo y=$y
