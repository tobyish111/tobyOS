# most shells execute /bin/sh; bash may execute itself
echo 'echo hi' > $TMP/no-shebang
chmod +x $TMP/no-shebang
$SH -c '$TMP/no-shebang'
