shopt -s expand_aliases
i=0; while [ $i -lt 100 ]; do eval "alias al_$i='echo a$i'"; i=$((i+1)); done
eval 'al_0; al_31; al_32; al_99'
unalias al_50; alias al_new='echo new'
eval 'al_new; al_99'
n=$(alias | grep -c "^alias al_"); echo "l1 [$n]"
x=$(alias al_77='echo in-subst'; al_1); echo "l2 [$x]"
alias zz='echo zz-still-here'; x=$(unalias zz; alias yy=1); eval zz; eval al_77
echo done
