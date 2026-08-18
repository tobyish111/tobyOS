case $SH in bash) exit ;; esac

x=$'\\D{%H:%M'  # leave off trailing }
echo x=${x@P}


# bash just ignores the missing }

# These shells don't understand @P
