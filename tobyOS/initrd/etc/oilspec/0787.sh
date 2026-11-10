cd $REPO_ROOT
compgen -W 'one two three'
echo --
compgen -W 'v1 v2 three' -A directory v
echo --
compgen -A directory -W 'v1 v2 three' v  # order doesn't matter
