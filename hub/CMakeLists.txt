# Muse Gadget firmware for the EvenG2-Muse-Hub-M5 hub.
# The SDK's cmake/project.cmake adds the SDK's main/ and components/ when it is
# included from a project outside the SDK tree.
cmake_minimum_required(VERSION 3.16)

set(MUSE_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/muse-gadget-sdk/esp32")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# ESP-IDF reads version.txt from the project root (hub/) unless PROJECT_VER is set.
file(STRINGS "${MUSE_SDK_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)

include("${MUSE_SDK_DIR}/cmake/project.cmake")
# Keep the SDK's app name so the SDK tools and web flasher find muse-gadget.bin.
project(muse-gadget)
include("${MUSE_SDK_DIR}/cmake/validate_config.cmake")
