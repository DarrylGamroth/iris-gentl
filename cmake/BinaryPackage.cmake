set(genapi_install_dir "${CMAKE_INSTALL_LIBDIR}/iris-gentl/genapi")
set(genapi_notice_dir "${CMAKE_INSTALL_DOCDIR}/genapi")
set_target_properties(iris PROPERTIES INSTALL_RPATH "$ORIGIN/iris-gentl/genapi")
file(RELATIVE_PATH lib_from_bin "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
set_target_properties(iris-capture iris-gentl PROPERTIES INSTALL_RPATH "$ORIGIN/${lib_from_bin}")

# Install vendor files as data so CMake does not relink or strip them.
set(genapi_checksums "")
foreach(module GCBase GenApi Log MathParser NodeMapData XmlParser)
    set(library "lib${module}_${genapi_compiler}_v3_5.so")
    set(source "${genapi_runtime_dir}/${library}")
    file(SHA256 "${source}" sha256)
    string(APPEND genapi_checksums "${sha256}  ${library}\n")
    install(FILES "${source}" DESTINATION "${genapi_install_dir}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GenApi-SHA256SUMS" "${genapi_checksums}")
install(FILES "${genapi_dir}/License_ReadMe.txt" "${CMAKE_CURRENT_BINARY_DIR}/GenApi-SHA256SUMS"
    DESTINATION "${genapi_notice_dir}")
install(DIRECTORY "${genapi_dir}/licenses" DESTINATION "${genapi_notice_dir}")

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME iris-gentl)
set(CPACK_PACKAGE_CONTACT "Iris GenTL maintainers <7671476+DarrylGamroth@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GenTL camera discovery, feature access, and capture")
set(CPACK_DEBIAN_PACKAGE_SECTION libs)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
include(CPack)
