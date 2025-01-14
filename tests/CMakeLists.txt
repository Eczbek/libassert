# Don't run tests when library is used with add_subdirectory
if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(WIN32)
      add_custom_command(
        TARGET libassert-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:cpptrace::cpptrace>
        $<TARGET_FILE_DIR:libassert-lib>
      )
    endif()
    include(CTest)

    set(
      demo_sources
      tests/demo/bar.cpp
      tests/demo/baz/demo.cpp
      tests/demo/demo.cpp
      tests/demo/foo.cpp
    )
    add_executable(demo ${demo_sources})
    target_link_libraries(demo PRIVATE libassert-lib)
    target_compile_options(
      demo
      PRIVATE
      "-DLIBASSERT_USE_MAGIC_ENUM"
      "-DLIBASSERT_LOWERCASE"
    )
    target_compile_features(
      demo
      PUBLIC cxx_std_20
    )
    target_compile_definitions(
      demo
      PUBLIC LIBASSERT_SAFE_COMPARISONS
    )

    add_executable(integration tests/integration/integration.cpp tests/integration/a.cpp tests/integration/x/a.cpp)
    # Temporary workaround for Visual Studio 2022 bug with __builtin_LINE() and __builtin_FILE()
    # https://developercommunity.visualstudio.com/t/__builtin_LINE-function-is-reporting-w/10439054?space=62&q=__builtin_function
    # TODO: Workaround in the header for end users?
    target_compile_features(
      integration
      PUBLIC cxx_std_20
    )
    target_link_libraries(integration PRIVATE libassert-lib)
    target_compile_options(
      integration
      PRIVATE
      "-DLIBASSERT_USE_MAGIC_ENUM"
      "-DLIBASSERT_LOWERCASE"
      "-DLIBASSERT_SAFE_COMPARISONS"
    )
    add_test(
      NAME integration
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
      COMMAND
      python3 run-tests.py $<TARGET_FILE:integration> ${CMAKE_BUILD_TYPE} ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_STANDARD}
    )

    set(
      dsym_targets
      demo
      integration
    )

    set(
      all_targets
      demo
      integration
    )

    set(
      unit_test_sources
      tests/unit/constexpr_contexts.cpp
      tests/unit/disambiguation.cpp
      tests/unit/literals.cpp
      tests/unit/test_public_utilities.cpp
      tests/unit/test_type_prettier.cpp
      tests/unit/tokens_and_highlighting.cpp
      tests/unit/type_handling.cpp
      tests/unit/stringify.cpp
    )
    foreach(test_file ${unit_test_sources})
      get_filename_component(test_name ${test_file} NAME_WE)
      list(APPEND all_targets ${test_name})
      add_executable(${test_name} ${test_file})
      target_link_libraries(${test_name} libassert-lib)
      add_test(NAME ${test_name} COMMAND ${test_name})
      list(APPEND dsym_targets ${test_name})
    endforeach(test_file ${unit_test_sources})
    add_executable(basic tests/basic/basic_test.cpp)
    target_link_libraries(basic PRIVATE libassert-lib)

    if(APPLE)
      foreach(target ${dsym_targets})
        add_custom_command(
          TARGET ${target}
          POST_BUILD
          COMMAND dsymutil $<TARGET_FILE:${target}>
        )
      endforeach(target ${dsym_targets})
    endif()

    foreach(target ${all_targets})
      target_compile_options(
        ${target_name}
        PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror=return-type -Wshadow -Wundef -Werror>
        $<$<CXX_COMPILER_ID:GNU>:-Wuseless-cast -Wnonnull-compare>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->
      )
    endforeach()
endif()
