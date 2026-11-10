# In a list's [k]=value only an unquoted `~` at the value's start (or
# after a `:`) is the home directory: `[1]="~"` and `[1]='~'` stay `~` --
# tsh expanded the quoted ones too (finding 495).
exec 2>&1
HOME=/home/hh
a=([1]="~" [2]=~ [3]='~' [4]=x~ [5]="~/p" [6]=~/q [7]=a:~ [8]=a:"~" [9]=\~)
declare -p a
b=("~" ~ '~')
declare -p b
