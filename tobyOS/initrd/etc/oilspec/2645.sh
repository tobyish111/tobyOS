case $SH in zsh) exit ;; esac

#$SH -c 'echo hostname=$HOSTNAME'

HOSTNAME=x $SH -c 'echo hostname=$HOSTNAME'
OSTYPE=x $SH -c 'echo ostype=$OSTYPE'
echo

#PS4=x $SH -c 'echo ps4=$PS4'

# OPTIND is special
#OPTIND=xx $SH -c 'echo optind=$OPTIND'
