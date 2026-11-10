case $SH in dash|mksh|ash|yash) exit 99;; esac
IFS=x
axb=1
echo "${!axb*}"
echo "${!axb*}"x
