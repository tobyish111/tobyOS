shopt -s globstar

mkdir directory
touch leaf.md
touch directory/leaf.md

echo **/*.* | sort
echo directory/**/*.md | sort
echo d**/*.md | sort
echo **y/*.md | sort
echo d**y/*.md | sort
