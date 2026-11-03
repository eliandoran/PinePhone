################################################################################
#
# noto-emoji
#
################################################################################

# Noto Emoji v2.048 (Emoji 16.0), as packaged by emoji-data
NOTO_EMOJI_VERSION = 16.0.0
NOTO_EMOJI_SOURCE = emoji-datasource-google-$(NOTO_EMOJI_VERSION).tgz
NOTO_EMOJI_SITE = https://registry.npmjs.org/emoji-datasource-google/-
# The images are Apache-2.0 (Noto), the package around them MIT
NOTO_EMOJI_LICENSE = Apache-2.0, MIT
NOTO_EMOJI_LICENSE_FILES = LICENSE

# Single code points only, "-fe0f" (emoji presentation) dropped from the name
define NOTO_EMOJI_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/emoji
	for f in $(@D)/img/google/64/*.png; do \
		name=$${f##*/}; name=$${name%.png}; name=$${name%-fe0f}; \
		case $$name in *-*) continue ;; esac; \
		$(INSTALL) -m 0644 $$f $(TARGET_DIR)/usr/share/emoji/$$name.png || exit 1; \
	done
endef

$(eval $(generic-package))
