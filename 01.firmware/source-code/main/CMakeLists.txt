set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c ${LV_DEMOS_SOURCES} 
    lvgl_port/show_jpg.c
    lvgl_port/lock_page.c
    lvgl_port/video_audio.c
    lvgl_port/main_page.c
    lvgl_port/photo_album.c
    lvgl_port/page1.c
    lvgl_port/game1.c
    lvgl_port/setting.c



    INCLUDE_DIRS . ${LV_DEMO_DIR} lvgl_port/include
    
    
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)