declare -A assoc=([k1]=foo [k2]='spam eggs')
declare -p assoc

# Bash 5.1 assoc=(key value). Bash 5.0 (including the currently tested 4.4)
# does not implement this.

assoc=(foo 'spam eggs')
declare -p assoc
