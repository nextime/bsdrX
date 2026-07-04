# OpenWRT package for bsdr_micrelay — the bsdrX router-side owner-mic relay.
#
# It captures a Bigscreen headset's mic RTP on the router and forwards it to a bsdrX host
# (bsdr_agent --sniff-remote), so the owner mic / voice control works over WiFi where the host
# itself can't see the headset's traffic.
#
# Build (from an OpenWRT buildroot/SDK):
#   1. Point BSDRX_SRC at your bsdrX checkout (the dir with tools/ and src/):
#        ln -s /path/to/bsdrX package/bsdr-micrelay/bsdrx   # or pass BSDRX_SRC=... to make
#      Simplest: copy this whole openwrt/bsdr-micrelay dir into <sdk>/package/ and set BSDRX_SRC.
#   2. make menuconfig   -> Network -> bsdr-micrelay  (M)
#   3. make package/bsdr-micrelay/compile BSDRX_SRC=/abs/path/to/bsdrX V=s
#   The .ipk lands in bin/packages/<arch>/ ; copy to the router and `opkg install`.

include $(TOPDIR)/rules.mk

PKG_NAME:=bsdr-micrelay
PKG_VERSION:=0.0.1
PKG_RELEASE:=1
PKG_MAINTAINER:=Stefy Lanza <stefy@nexlab.net>
PKG_LICENSE:=GPL-3.0-or-later

include $(INCLUDE_DIR)/package.mk

# Path to the bsdrX source tree (override on the make command line).
BSDRX_SRC?=$(CURDIR)/bsdrx

define Package/bsdr-micrelay
  SECTION:=net
  CATEGORY:=Network
  TITLE:=bsdrX router-side owner-mic relay
  URL:=https://git.nexlab.net/nextime/bsrdx
  DEPENDS:=+libpcap
endef

define Package/bsdr-micrelay/description
  Captures a Bigscreen VR headset's microphone RTP on the router (the gateway sees all of the
  headset's traffic) and forwards it to a bsdrX host running `bsdr_agent --sniff-remote <port>`.
  Enables the owner microphone and voice computer-control over WiFi, where the host cannot
  otherwise intercept the headset's mic-to-cloud stream.
endef

# Copy just the two sources + header the relay needs out of the bsdrX tree.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(BSDRX_SRC)/tools/bsdr_micrelay.c   $(PKG_BUILD_DIR)/
	$(CP) $(BSDRX_SRC)/src/micsniff_capture.c  $(PKG_BUILD_DIR)/
	$(CP) $(BSDRX_SRC)/src/micsniff_capture.h  $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -DBSDR_HAVE_PCAP=1 -I$(PKG_BUILD_DIR) \
		$(PKG_BUILD_DIR)/bsdr_micrelay.c $(PKG_BUILD_DIR)/micsniff_capture.c \
		$(TARGET_LDFLAGS) -lpcap -o $(PKG_BUILD_DIR)/bsdr_micrelay
endef

define Package/bsdr-micrelay/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bsdr_micrelay $(1)/usr/bin/bsdr_micrelay
endef

$(eval $(call BuildPackage,bsdr-micrelay))
