# The tree walkers and symbolic links. A tree with a link to a file, to a
# directory outside it, to its own ancestor and a dangling one: nothing is
# followed out of the tree (rm -rf, chmod -R, du), nothing outside is
# touched, and cp -r and tar keep links as links. With links real and stat
# following them, every walker that used stat would have walked out of the
# tree (logs/b15 walk).
exec 2>&1
cd /tmp; rm -rf sp293 sp293o sp293c sp293x sp293.tar; mkdir -p sp293/sub sp293o
echo keep > sp293o/precious
echo a > sp293/sub/a
ln -s ../sp293o sp293/out
ln -s .. sp293/sub/up
ln -s sub/a sp293/la
ln -s nosuch sp293/dang
echo "== du"; du -s sp293 | sed 's/[0-9]*/N/'
echo "== find"; find sp293 | sort
echo "== find -type l"; find sp293 -type l | sort
echo "== find -L -type f"; find -L sp293 -maxdepth 2 -type f | sort
echo "== find -xtype l"; find sp293 -xtype l
echo "== cp -r"; cp -r sp293 sp293c; find sp293c | sort
readlink sp293c/out sp293c/sub/up sp293c/la sp293c/dang
echo "== tar"; tar -cf sp293.tar sp293 && mkdir sp293x && cd sp293x &&
  tar -xf ../sp293.tar && cd .. && find sp293x | sort && readlink sp293x/sp293/out
echo "== chmod -R"; chmod -R 700 sp293; ls -l sp293o/precious | cut -c1-10
echo "== rm -rf"; rm -rf sp293 sp293c sp293x sp293.tar; ls sp293; cat sp293o/precious
rm -rf sp293o
echo end
