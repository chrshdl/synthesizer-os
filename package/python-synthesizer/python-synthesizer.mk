################################################################################
# python-synthesizer
################################################################################

# Use the specific version tag
PYTHON_SYNTHESIZER_VERSION = 0.1.8
PYTHON_SYNTHESIZER_SITE = $(call github,chrshdl,synthesizer,v$(PYTHON_SYNTHESIZER_VERSION))

PYTHON_SYNTHESIZER_LICENSE = MIT
PYTHON_SYNTHESIZER_LICENSE_FILES = LICENSE

PYTHON_SYNTHESIZER_SETUP_TYPE = pep517
PYTHON_SYNTHESIZER_DEPENDENCIES = python-pygame-261 python-numpy

define PYTHON_SYNTHESIZER_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL)/package/python-synthesizer/synthesizer.service \
		$(TARGET_DIR)/usr/lib/systemd/system/synthesizer.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf /usr/lib/systemd/system/synthesizer.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/synthesizer.service
endef


$(eval $(python-package))

