# Package metadata
set(CPACK_PACKAGE_NAME "MyNVR")
set(CPACK_PACKAGE_FILE_NAME "nvr")
set(CPACK_PACKAGE_DIRECTORY ${PROJECT_BINARY_DIR})

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DISPLAY_NAME}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_URL}")

# Version from main project
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

set(CPACK_VERBATIM_VARIABLES TRUE)

if(WIN32)
  include(InstallRequiredSystemLibraries)

  # Windows: MSI installer via WiX
  set(CPACK_GENERATOR ZIP WIX)
  #set(CPACK_INNOSETUP_DEFINE_MyMacro "Hello, World!")
  #set(CPACK_INNOSETUP_EXTRA_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/innosetup/installer.iss")

  set(CPACK_PACKAGE_FILE_NAME "nvr-win64")

  # WiX configuration
  # Upgrade GUID must never change
  set(CPACK_WIX_UPGRADE_GUID "755C43AF-1768-455A-9DB1-57AD0DA8B84F")

  # Program menu
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "${CPACK_PACKAGE_NAME}")
  set(CPACK_PACKAGE_EXECUTABLES "nvr" "NVR Service")

  # Install per-machine (requires admin)
  set(CPACK_WIX_INSTALL_SCOPE "perMachine")

  # UI customization
  set(CPACK_WIX_UI_REF "WixUI_InstallDir")

  # Extensions
  list(APPEND CPACK_WIX_EXTENSIONS WixUtilExtension WixUIExtension)

  list(APPEND CPACK_WIX_PATCH_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/wix/custom_actions.wxs
  )

endif()

# Include CPack
include(CPack)

# Custom target for building the package
add_custom_target(wix
  COMMAND ${CMAKE_COMMAND} --build . --target package
  DEPENDS nvr
  COMMENT "Building WiX installer..."
  VERBATIM
)
