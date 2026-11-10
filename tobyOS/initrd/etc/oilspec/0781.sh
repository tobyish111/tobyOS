cd $REPO_ROOT
compgen -o plusdirs -W 'a b1 b2' b | sort
echo ---
compgen -o dirnames b | sort
