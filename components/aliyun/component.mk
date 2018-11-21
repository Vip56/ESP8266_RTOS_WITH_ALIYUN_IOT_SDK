#
# Component Makefile
#
COMPONENT_ADD_INCLUDEDIRS := \
    include \
    include/exports \
    include/imports \

COMPONENT_SRCDIRS := \
    platform/os/espressif \
    platform/ssl/mbedtls 

# link libiot_sdk.a
LIBS += iot_sdk
COMPONENT_ADD_LDFLAGS += -L $(COMPONENT_PATH)/lib $(addprefix -l,$(LIBS))
