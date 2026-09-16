# Target-based warning / analysis configuration for first-party code.

function(routefabric_apply_compile_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /EHsc
      /MP)
    if(ROUTEFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(ROUTEFABRIC_ENABLE_ANALYZE)
      # The static analyzer runs over first-party code only. External headers are
      # excluded through the standard /external mechanism rather than by
      # disabling diagnostics globally.
      target_compile_options(${target} PRIVATE
        /analyze
        /analyze:external:W0
        /external:anglebrackets
        /external:W0)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion)
    if(ROUTEFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(ROUTEFABRIC_ENABLE_COVERAGE)
      target_compile_options(${target} PRIVATE --coverage)
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endif()
endfunction()
