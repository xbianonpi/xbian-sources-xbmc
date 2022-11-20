#.rst:
# FindTexturePacker
# -----------------
# Finds the TexturePacker
#
# If WITH_TEXTUREPACKER is defined and points to a directory,
# this path will be used to search for the Texturepacker binary
#
#
# This will define the following (imported) targets::
#
#   TexturePacker::TexturePacker   - The TexturePacker executable

if(NOT TARGET TexturePacker::TexturePacker)
  if(KODI_DEPENDSBUILD)
    get_filename_component(_tppath "${NATIVEPREFIX}/bin" ABSOLUTE)
    find_program(TEXTUREPACKER_EXECUTABLE NAMES "${APP_NAME_LC}-TexturePacker" TexturePacker
                                          HINTS ${_tppath})

    add_executable(TexturePacker::TexturePacker IMPORTED GLOBAL)
    set_target_properties(TexturePacker::TexturePacker PROPERTIES
                                                       IMPORTED_LOCATION "${TEXTUREPACKER_EXECUTABLE}")
  elseif(WIN32)
    get_filename_component(_tppath "${DEPENDENCIES_DIR}/tools/TexturePacker" ABSOLUTE)
    find_program(TEXTUREPACKER_EXECUTABLE NAMES "${APP_NAME_LC}-TexturePacker.exe" TexturePacker.exe
                                          HINTS ${_tppath})

    add_executable(TexturePacker::TexturePacker IMPORTED GLOBAL)
    set_target_properties(TexturePacker::TexturePacker PROPERTIES
                                                       IMPORTED_LOCATION "${TEXTUREPACKER_EXECUTABLE}")
  else()
    if(WITH_TEXTUREPACKER)
      get_filename_component(_tppath ${WITH_TEXTUREPACKER} ABSOLUTE)
      get_filename_component(_tppath ${_tppath} DIRECTORY)
      find_program(TEXTUREPACKER_EXECUTABLE NAMES "${APP_NAME_LC}-TexturePacker" TexturePacker
                                            HINTS ${_tppath})

      include(FindPackageHandleStandardArgs)
      find_package_handle_standard_args(TexturePacker "Could not find '${APP_NAME_LC}-TexturePacker' or 'TexturePacker' executable in ${_tppath} supplied by -DWITH_TEXTUREPACKER. Make sure the executable file name matches these names!"
                                        TEXTUREPACKER_EXECUTABLE)
      if(TEXTUREPACKER_FOUND)
        add_executable(TexturePacker::TexturePacker IMPORTED GLOBAL)
        set_target_properties(TexturePacker::TexturePacker PROPERTIES
                                                           IMPORTED_LOCATION "${TEXTUREPACKER_EXECUTABLE}")
      endif()
      mark_as_advanced(TEXTUREPACKER)
    else()
      add_subdirectory(${CMAKE_SOURCE_DIR}/tools/depends/native/TexturePacker build/texturepacker)
      add_executable(TexturePacker::TexturePacker ALIAS TexturePacker)
    endif()
  endif()
endif()
