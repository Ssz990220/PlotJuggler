# Platform-neutral Luau package config for the PJ4 pixi build.
# Replicates the component graph the Conan luau package exposed:
#   Common (StringUtils: format/hashRange) — referenced by all others
#   Ast      -> Common
#   Compiler -> Ast
#   VM       -> Common (+ libm on unix)
#   CodeGen  -> VM
# find_library resolves libLuau.<C>.a (unix) or Luau.<C>.lib (windows) alike.

if(TARGET Luau::VM)
  return()
endif()

get_filename_component(_luau_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_luau_inc "${_luau_prefix}/include")

foreach(_c Common Ast Compiler VM CodeGen)
  find_library(_luau_${_c}_lib
    NAMES Luau.${_c} libLuau.${_c}
    HINTS "${_luau_prefix}/lib"
    NO_DEFAULT_PATH REQUIRED)
  add_library(Luau::${_c} STATIC IMPORTED)
  set_target_properties(Luau::${_c} PROPERTIES
    IMPORTED_LOCATION "${_luau_${_c}_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_luau_inc}")
endforeach()

set_property(TARGET Luau::Ast      APPEND PROPERTY INTERFACE_LINK_LIBRARIES Luau::Common)
set_property(TARGET Luau::Compiler APPEND PROPERTY INTERFACE_LINK_LIBRARIES Luau::Ast)
set_property(TARGET Luau::VM       APPEND PROPERTY INTERFACE_LINK_LIBRARIES Luau::Common)
set_property(TARGET Luau::CodeGen  APPEND PROPERTY INTERFACE_LINK_LIBRARIES Luau::VM)
if(UNIX)
  set_property(TARGET Luau::VM APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
endif()

set(Luau_FOUND TRUE)
