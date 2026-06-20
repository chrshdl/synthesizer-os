################################################################################
# synthesizer-audio-engine
#
# A low-latency ALSA audio engine for the additive synthesizer, implemented in
# C++17 and exposed as a shared library loaded by the Python ctypes bindings.
################################################################################

SYNTHESIZER_AUDIO_ENGINE_VERSION = 1.0.0
SYNTHESIZER_AUDIO_ENGINE_SITE = $(BR2_EXTERNAL_SYNTHESIZER_OS_PATH)/package/synthesizer-audio-engine/src
SYNTHESIZER_AUDIO_ENGINE_SITE_METHOD = local

SYNTHESIZER_AUDIO_ENGINE_LICENSE = MIT
SYNTHESIZER_AUDIO_ENGINE_LICENSE_FILES = $(BR2_EXTERNAL_SYNTHESIZER_OS_PATH)/LICENSE

# Depend on ALSA lib (libasound) which is always present in the BSP
SYNTHESIZER_AUDIO_ENGINE_DEPENDENCIES = alsa-lib

define SYNTHESIZER_AUDIO_ENGINE_BUILD_CMDS
	$(TARGET_CXX) $(TARGET_CXXFLAGS) $(TARGET_LDFLAGS) \
		-std=c++17 -O3 -ffast-math \
		-shared -fPIC \
		-o $(@D)/libsynthengine.so \
		$(@D)/synthesizer_audio_engine.cpp \
		-lasound -lm -lpthread \
		-Wl,-soname,libsynthengine.so
endef

define SYNTHESIZER_AUDIO_ENGINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libsynthengine.so \
		$(TARGET_DIR)/usr/lib/libsynthengine.so
endef

$(eval $(generic-package))
