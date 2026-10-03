# In the `declare -p` listing a dynamic variable (RANDOM, SECONDS, LINENO,
# BASHPID, COMP_WORDBREAKS, ...) shows the value it was last read as, or
# none if it never was; `set` lists it only once read; -v is true; TERM
# defaults to dumb and OPTIND is an integer. tsh left them all out.
# (batch 12; probe p89 in C:\t\b12)
exec 2>&1
declare -p | sed -E 's/=.*//' | grep -vE ' (SH|PWD|OLDPWD|HOME|USER|LOGNAME|SHELL|SSH_[A-Z_]*|MAIL|HOSTNAME|PPID|UID|EUID|SHLVL|BASH|BASH_VERSINFO|MACHTYPE|OSTYPE|BASH_ARGV0|BASH_SOURCE|_)$'
echo "== set"
set | sed -E 's/=.*//' | grep -vE '^(SH|PWD|OLDPWD|HOME|USER|LOGNAME|SHELL|SSH_[A-Z_]*|MAIL|HOSTNAME|PPID|UID|EUID|SHLVL|BASH|BASH_VERSINFO|MACHTYPE|OSTYPE|BASH_SOURCE|_)$'
echo "TERM=$TERM"; declare -p OPTIND
