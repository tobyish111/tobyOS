cat <<- EOF
	outside
	$(cat <<- INSIDE
		inside
INSIDE
)
EOF
