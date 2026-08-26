# mksh accepts ${s:0} and ${s:$zero} but not ${s:zero}
# zsh says unrecognized modifier 'z'
s='abcd'
zero=0
echo ${s:zero} ${s:zero+0} ${s:zero+1:zero+1}
