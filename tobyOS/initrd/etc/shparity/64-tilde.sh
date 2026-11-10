# A TILDE THAT WAS WRITTEN IN THE WORD EXPANDS; ONE THAT ARRIVED FROM AN
# EXPANSION IS DATA. The old test was one flag for the whole word, so as
# soon as any part of it expanded, every tilde in it stopped expanding --
# including the one the script typed.
HOME=/home/bar
a=~
echo A=$a
b=~/x
echo B=$b
c=p:~
echo C=$c
d=~:${undef-y}
echo D=$d
e=~:${undef-~:~}
echo E=$e
f=${undef-~}
echo F=$f
g='~'
echo G=$g
h="~"
echo H=$h
v='~'
i=$v
echo I=$i
echo J=~
echo K=~/x
echo done
