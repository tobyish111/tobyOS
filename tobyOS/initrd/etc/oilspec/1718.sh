# globskipdots is enabled by default in bash >=5.2
# for bash <5.2 this pattern is a common way to match dotfiles but not . or ..
shopt -u globskipdots
touch .env
GLOBIGNORE=.:..
echo .*
GLOBIGNORE=
echo .* | sort
