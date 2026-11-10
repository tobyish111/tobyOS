IFS=':%'
compgen -W '$(echo "spam:eggs%ham cheese")'
