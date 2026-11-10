mkdir -p $TMP/compgen2
touch $TMP/compgen2/{PA,Q}_FILE
cd $TMP/compgen2  # depends on previous test above!
PA_FUNC() { echo P; }
Q_FUNC() { echo Q; }
compgen -A function -A variable -A file PA
