a[0]=x
a[5]=y
a[10]=z
[[ -v a[-1] ]] && echo 'a has -1'
[[ -v a[-2] ]] && echo 'a has -2'
[[ -v a[-5] ]] && echo 'a has -5'
[[ -v a[-6] ]] && echo 'a has -6'
[[ -v a[-10] ]] && echo 'a has -10'
[[ -v a[-11] ]] && echo 'a has -11'
