# walk.sh: the native tree walkers and symbolic links (item 3). A tree
# with a link to a file, to a directory outside it, to its own ancestor and
# a dangling one: nothing may be followed out of the tree, nothing outside
# may be touched, and the links must survive copies and archives as links.
exec 2>&1
cd /tmp; rm -rf wk wkout wkcp wkx wk.tar; mkdir -p wk/sub wkout
echo keep > wkout/precious
echo a > wk/sub/a
ln -s ../wkout wk/out
ln -s .. wk/sub/up
ln -s sub/a wk/la
ln -s nosuch wk/dang
echo "== du"; du -s wk | sed 's/[0-9]*/N/'
echo "== find"; find wk | sort
echo "== find -L -type f"; find -L wk -maxdepth 2 -type f | sort
echo "== cp -r"; cp -r wk wkcp; find wkcp | sort; readlink wkcp/out wkcp/sub/up wkcp/la wkcp/dang
echo "== tar"; tar -cf wk.tar wk && mkdir wkx && cd wkx && tar -xf ../wk.tar && cd .. && find wkx | sort && readlink wkx/wk/out
echo "== tar -tv"; tar -tvf wk.tar | grep -- '->' | sort
echo "== chmod -R"; chmod -R 700 wk; ls -l wkout/precious | cut -c1-10
echo "== rm -rf"; rm -rf wk wkcp wkx wk.tar; ls wk 2>&1; cat wkout/precious
rm -rf wkout
echo end
