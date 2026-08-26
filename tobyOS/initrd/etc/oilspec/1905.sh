icu_cppflags=`((pkg-config --cflags icu-uc icu-i18n) ||
                  (pkgconf --cflags icu-uc icu-i18n) ||
                  (icu-config --cppflags)) 2>/dev/null`
echo bye
