# If it were shortest, then you would just replace the first <html>
s='begin <html></html> end'
echo ${s/<*>/[]}
