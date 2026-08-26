# https://github.com/git-for-windows/git-sdk-64/blob/main/usr/bin/zdiff#L136

gzip_status=$(
  exec 4>&1
  (gzip -cdfq -- "$file1" 4>&-; echo $? >&4) 3>&- |
      ((gzip -cdfq -- "$file2" 4>&-
        echo $? >&4) 3>&- 5<&- </dev/null |
       eval "$cmp" /dev/fd/5 - >&3) 5<&0
)
echo bye
