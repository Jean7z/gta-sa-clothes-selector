LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# The module name matches AML's library lookup name for this mod.
LOCAL_MODULE := AML_PSDK_SelectorLauncher
LOCAL_CPP_EXTENSION := .cpp .cc

# Menu Kit is compiled into this library (source-integrated), not loaded as a
# separate mod. This hides GetMenuAPI from the dynamic symbol table so the mod
# cannot be mistaken for the standalone Menu Kit library.
LOCAL_CFLAGS += -DMENUKIT_EMBEDDED

# mod/config.cpp is deliberately absent: that is Menu Kit's own standalone-mod
# config, and it is not part of the embedded build.
LOCAL_SRC_FILES := main.cpp mod/menu.cpp mod/logger.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/psdk $(LOCAL_PATH)/mod
LOCAL_CXXFLAGS := -Os -ffunction-sections -fdata-sections -DNDEBUG -std=c++17
LOCAL_LDFLAGS := -Wl,--gc-sections
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)