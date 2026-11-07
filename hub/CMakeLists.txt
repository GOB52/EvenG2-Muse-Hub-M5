# Muse Gadget firmware for the EvenG2-Muse-Hub-M5 hub.
# The SDK's cmake/project.cmake adds the SDK's main/ and components/ when it is
# included from a project outside the SDK tree.
cmake_minimum_required(VERSION 3.16)

set(MUSE_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/muse-gadget-sdk/esp32")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# The SDK's version (999.0.0) followed by the hub's own version. OTA is on for
# Muse boards and installs only an image whose MAJOR.MINOR.PATCH is newer, so
# keeping 999.0.0 in front stops an official Muse image from replacing the hub.
set(PROJECT_VER "999.0.0+hub.0.0.1")

include("${MUSE_SDK_DIR}/cmake/project.cmake")
# Keep the SDK's app name so the SDK tools and web flasher find muse-gadget.bin.
project(muse-gadget)
include("${MUSE_SDK_DIR}/cmake/validate_config.cmake")
