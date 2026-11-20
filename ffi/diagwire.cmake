# Builds the C interface to the diagnostic protocol for rust_target, or for the host when it is
# empty, as the target diagwire that carries the library, its header and the build order.
function(diagwire_build rust_target)
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/diagwire)
  if(rust_target)
    set(built ${dir}/${rust_target}/ffi/libdiagwire_ffi.a)
    set(cargo_target --target ${rust_target})
  else()
    set(built ${dir}/ffi/libdiagwire_ffi.a)
  endif()
  set(lib ${dir}/libdiagwire.a)
  # Only the C interface stays global: the library's own memcpy, memset and soft-float helpers
  # would otherwise replace the C library's for everything linked with it.
  add_custom_target(diagwire_build
    COMMAND ${CMAKE_COMMAND} -E env DIAGWIRE_HEADER=${dir}/diagwire.h
      cargo build --profile ffi ${cargo_target}
      --manifest-path ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Cargo.toml --target-dir ${dir}
    COMMAND ${CMAKE_OBJCOPY} --wildcard --keep-global-symbol=diag_* ${built} ${lib}.new
    # The library's timestamp stays unless its bytes change.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${lib}.new ${lib}
    BYPRODUCTS ${lib} ${dir}/diagwire.h USES_TERMINAL)
  add_library(diagwire INTERFACE)
  add_dependencies(diagwire diagwire_build)
  target_include_directories(diagwire INTERFACE ${dir})
  target_link_libraries(diagwire INTERFACE ${lib})
endfunction()
