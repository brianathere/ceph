## extended block device plugins

set(extblkdev_plugin_dir ${CEPH_INSTALL_PKGLIBDIR}/extblkdev)

add_subdirectory(vdo)
if(NOT APPLE)
  # fcm uses libblkid (Linux-only; HAVE_BLKID off on macOS)
  add_subdirectory(fcm)
endif()

add_library(extblkdev STATIC ExtBlkDevPlugin.cc)

if(LINUX)
  find_package(cap REQUIRED)
  target_link_libraries(extblkdev cap)
endif()

set(extblkdev_plugin_targets ceph_ebd_vdo)
if(NOT APPLE)
  list(APPEND extblkdev_plugin_targets ceph_ebd_fcm)
endif()
add_custom_target(extblkdev_plugins DEPENDS ${extblkdev_plugin_targets})
