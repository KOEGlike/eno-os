# Create the UI sources as a library if any sources are provided.
if (PROJECT_SOURCES)
    add_library(lib-ui ${PROJECT_SOURCES})
else ()
    add_library(lib-ui INTERFACE)
endif ()

# You may use this check to add configuration when compiling for the Editor preview,
# or for your target.
if (LV_EDITOR_PREVIEW)
    # things for the Preview
else ()
    # things for your target

    # Link with Zephyr interface to inherit compiler flags and settings
    target_link_libraries(lib-ui INTERFACE zephyr_interface)

    # Include LVGL headers from Zephyr's module path
    target_include_directories(lib-ui INTERFACE
        ${CMAKE_BINARY_DIR}/zephyr/include/generated/lvgl
        ${ZEPHYR_LVGL_MODULE_DIR}
    )
endif ()