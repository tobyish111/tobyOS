# POSIX 2.6.1 ~user, live now that the repo ships /etc/passwd. bash reads
# the database and does NOT stat the directory; an unknown name stays
# exactly as written; quoting disables the expansion entirely.
HOME=/home/probe
echo ~root
echo ~toby
echo ~nosuchuser
echo ~root/sub
x=~root:~:~toby
echo "x=$x"
y=${undef-~root:~}
echo "y=$y"
echo "~root"
echo '~root'
compgen -A user
compgen -A user to
compgen -A user zz
echo "c=$?"
echo end
