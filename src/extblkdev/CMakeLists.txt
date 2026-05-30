# The fcm plugin uses libblkid (Linux-only; HAVE_BLKID is OFF on macOS). Gate it
# out on Apple so the OSD links without blkid.
if(NOT APPLE)
  add_subdirectory(fcm)
endif()

set(SOURCES
   ExtBlkDevInterface.cc
   ExtBlkDevPlugin.cc)

add_library(extblkdev STATIC ${SOURCES})
target_link_libraries(extblkdev legacy-option-headers)
add_dependencies(extblkdev legacy-option-headers)

if(WITH_SYSTEM_LIBVERBS)
endif()

add_custom_target(extblkdev_plugins)
if(NOT APPLE)
  add_dependencies(extblkdev_plugins ceph_ebd_fcm)
endif()
