file(GLOB DUNE_BZIP2_FILES
  external/libraries/bzip2/*.c)

set_source_files_properties(${DUNE_BZIP2_FILES}
  PROPERTIES COMPILE_FLAGS "${DUNE_CXX_FLAGS} ${DUNE_CXX_FLAGS_STRICT}")

list(APPEND DUNE_EXTERNAL_FILES ${DUNE_BZIP2_FILES})
