case $SH in mksh) echo 'weird bug'; exit ;; esac

# the mu character is U+03BC

printf '%x\n' \'μ
printf '%u\n' \'μ
printf '%o\n' \'μ
echo

u3=三
# u4=😘

printf '%x\n' \'$u3
printf '%u\n' \'$u3
printf '%o\n' \'$u3
echo

# mksh DOES respect unicode on the new Debian bookworm.
# but even building the SAME SOURCE from scratch, somehow it doesn't on Ubuntu 8.
# TBH I should probably just upgrade the mksh version.
#
# $ ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
# 
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ cat /etc/os-release
# NAME="Ubuntu"
# VERSION="18.04.5 LTS (Bionic Beaver)"
# ID=ubuntu
# ID_LIKE=debian
# PRETTY_NAME="Ubuntu 18.04.5 LTS"
# VERSION_ID="18.04"
# HOME_URL="https://www.ubuntu.com/"
# SUPPORT_URL="https://help.ubuntu.com/"
# BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
# PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
# VERSION_CODENAME=bionic
# UBUNTU_CODENAME=bionic
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ env|egrep 'LC|LANG'
# LANG=en_US.UTF-8
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ LC_CTYPE=C.UTF-8 ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ LANG=C.UTF-8 ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ LC_ALL=C.UTF-8 ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ LC_ALL=en_US.UTF-8 ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
# andy@lenny:~/wedge/oils-for-unix.org/pkg/mksh/R52c$ LC_ALL=en_US.utf-8 ./mksh -c 'printf "%u\n" \"$1' dummy $'\u03bc'
# printf: warning: : character(s) following character constant have been ignored
# 206
