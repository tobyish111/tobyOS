case $SH in zsh) exit ;; esac

x='a b'

z=command
$z readonly c=$x
echo $c

z=c
${z}ommand readonly d=$x
echo $d
