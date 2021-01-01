
if(LLVM_DISTRIBUTION_COMPONENTS)
  if(LLVM_ENABLE_IDE)
    message(FATAL_ERROR "LLVM_DISTRIBUTION_COMPONENTS cannot be specified with multi-configuration generators (i.e. Xcode or Visual Studio)")
  endif()
endif()

function(llvm_distribution_add_targets)
  add_custom_target(distribution)
  set_target_properties(distribution PROPERTIES FOLDER "LLVM")
  add_custom_target(install-distribution)
  set_target_properties(install-distribution PROPERTIES FOLDER "LLVM")
  add_custom_target(install-distribution-stripped)
  set_target_properties(install-distribution-stripped PROPERTIES FOLDER "LLVM")

  foreach(target ${LLVM_DISTRIBUTION_COMPONENTS}
      ${LLVM_RUNTIME_DISTRIBUTION_COMPONENTS})
    if(TARGET ${target})
      add_dependencies(distribution ${target})
    else()
      message(SEND_ERROR "Specified distribution component '${target}' doesn't have a target")
    endif()

    if(TARGET install-${target})
      add_dependencies(install-distribution install-${target})
    else()
      message(SEND_ERROR "Specified distribution component '${target}' doesn't have an install target")
    endif()

    if(TARGET install-${target}-stripped)
      add_dependencies(install-distribution-stripped install-${target}-stripped)
    else()
      message(SEND_ERROR
              "Specified distribution component '${target}' doesn't have an install-stripped target."
              " Its installation target creation should be changed to use add_llvm_install_targets,"
              " or you should manually create the 'install-${target}-stripped' target.")
    endif()
  endforeach()
endfunction()
