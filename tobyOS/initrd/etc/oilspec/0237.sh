# Note: bash-5.2 has a bug that the tilde doesn't expand on the right hand side
# of [key]=value.  This problem doesn't happen in bash-3.1..5.1 and bash-5.3.
HOME=/home/user
declare -A a
declare -A a=(['home']=~ ['hello']=~:~:~)
echo "${a['home']}"
echo "${a['hello']}"
