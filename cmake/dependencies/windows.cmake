# windows specific dependencies

# Make sure MinHook is installed
find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED)
find_path(MINHOOK_INCLUDE_DIR MinHook.h PATH_SUFFIXES include REQUIRED)

add_library(minhook::minhook STATIC IMPORTED)
set_property(TARGET minhook::minhook PROPERTY IMPORTED_LOCATION ${MINHOOK_LIBRARY})
target_include_directories(minhook::minhook INTERFACE ${MINHOOK_INCLUDE_DIR})



find_library(MINIZIP_LIBRARY libminizip.a REQUIRED)
find_path(MINIZIP_INCLUDE_DIR minizip/unzip.h PATH_SUFFIXES include REQUIRED)

# minizip may require bzip2 if compiled with bzip2 support
# Try to find bzip2 library (non-required, as it might not be needed if minizip was built without bzip2)
find_library(BZ2_LIBRARY 
    NAMES libbz2.a bz2 libbz2
    PATHS 
        ${CMAKE_PREFIX_PATH}/lib
        ${CMAKE_PREFIX_PATH}/lib64
        /usr/lib
        /usr/local/lib
        ${CMAKE_SYSTEM_PREFIX_PATH}/lib
    NO_DEFAULT_PATH
)
if(BZ2_LIBRARY)
    add_library(bz2::bz2 STATIC IMPORTED)
    set_property(TARGET bz2::bz2 PROPERTY IMPORTED_LOCATION ${BZ2_LIBRARY})
    message(STATUS "Found bzip2 library: ${BZ2_LIBRARY}")
else()
    # If not found, try linking by name (common in MSYS2/MinGW)
    message(STATUS "bzip2 library not found, will try linking by name")
endif()

add_library(minizip::minizip STATIC IMPORTED)
set_property(TARGET minizip::minizip PROPERTY IMPORTED_LOCATION ${MINIZIP_LIBRARY})
target_include_directories(minizip::minizip INTERFACE ${MINIZIP_INCLUDE_DIR})