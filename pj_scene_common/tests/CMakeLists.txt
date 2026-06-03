if(PJ_BUILD_TESTS)
  find_package(GTest REQUIRED)

  add_executable(scene_common_test
    scene_common_test.cpp
  )

  set_target_properties(scene_common_test PROPERTIES
    AUTOMOC ON
  )

  target_compile_options(scene_common_test PRIVATE
    ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
  )

  target_link_libraries(scene_common_test PRIVATE
    pj_scene_common
    GTest::gtest
  )

  add_test(NAME scene_common_test COMMAND scene_common_test)
endif()
