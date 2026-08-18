case $SH in dash|zsh|ash) exit ;; esac

shopt -s extglob

x='foo()' 
echo 1 ${x%*(foo|bar)'()'}
echo 2 ${x%%*(foo|bar)'()'}
echo 3 ${x#*(foo|bar)'()'}
echo 4 ${x##*(foo|bar)'()'}
