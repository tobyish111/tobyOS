HOME=/home/bar
x=~
echo ${x//~/~root}

# gah there is some expansion, what the hell
echo ${HOME//~/~root}

x=[$HOME]
echo ${x//~/~root}
