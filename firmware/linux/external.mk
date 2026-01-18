# Makefiles used by all subprojects

define LINUX_INSTALL_NV3007
	$(PROJECT_VARIANT_DIR)/linux-post-patch.sh $(@D)
endef
LINUX_POST_PATCH_HOOKS += LINUX_INSTALL_NV3007