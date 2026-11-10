# Under <<- a line is compared with the delimiter as it stands before its
# tabs are stripped, so a quoted delimiter that begins with a tab, <<-'	END',
# ends at its own line -- tsh stripped first and read on (finding 447).
exec 2>&1
cat <<-'	END'
	hello
	END
echo end1
cat <<-END
	there
	END
echo end2
