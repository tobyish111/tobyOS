case $SH in dash|mksh|zsh) exit ;; esac

# https://oilshell.zulipchat.com/#narrow/stream/307442-nix/topic/Replacing.20bash.20with.20osh.20in.20Nixpkgs.20stdenv

(argv.py ${!hooksSlice+"${!hooksSlice}"})

hooksSlice=x

argv.py ${!hooksSlice+"${!hooksSlice}"}

declare -a hookSlice=()

argv.py ${!hooksSlice+"${!hooksSlice}"}

foo=42
bar=43

declare -a hooksSlice=(foo bar spam eggs)

argv.py ${!hooksSlice+"${!hooksSlice}"}


# Bash 4.4 has a bug that ${!undef-} successfully generates an empty word.
#
# ## BUG bash STDOUT:
# []
# []
# []
# ['42']
# ## END
