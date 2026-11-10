case $SH in bash) exit ;; esac

mkdir directory-1
mkdir directory-2
touch directory-2/leaf-2.md
ln -s -T ../directory-2 directory-1/symlink

echo **/*.* | sort
echo ***/*.* | sort
