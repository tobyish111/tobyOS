cd $TMP
echo 'echo hi' > no-shebang
chmod +x no-shebang
"$SH" -c './no-shebang'
