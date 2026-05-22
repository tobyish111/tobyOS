PROJECT_DIR := tobyOS

.PHONY: help all build iso rebuild qemu run quickiso fullboot clean \
        run-uefi run-installed run-installed-uefi wipe-disk usb-image inspect \
        sdk-pack m17test m20test m22shutdowntest m24dtest m34test m35test \
        m36test sectest posixshtest

help all build iso rebuild qemu run quickiso fullboot clean \
run-uefi run-installed run-installed-uefi wipe-disk usb-image inspect \
sdk-pack m17test m20test m22shutdowntest m24dtest m34test m35test \
m36test sectest posixshtest:
	$(MAKE) -C $(PROJECT_DIR) $@
