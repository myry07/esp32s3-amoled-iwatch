#include "ui.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "esp_log.h"

#include <dirent.h>
#include <sys/stat.h>

static void list_spiffs_files(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE("spiffs", "Failed to open dir: %s", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ESP_LOGI("spiffs", "DIR : %s", full_path);
            } else {
                ESP_LOGI("spiffs", "FILE: %s (size=%ld)", full_path, st.st_size);
            }
        } else {
            ESP_LOGW("spiffs", "stat failed for %s", full_path);
        }
    }

    closedir(dir);
}

void app_main(void)
{
    if (bsp_spiffs_mount() == ESP_OK)
    {
        ESP_LOGI("main", "spiffs mounted");
        list_spiffs_files("/spiffs");
    }
    else
    {
        ESP_LOGE("main", "spiffs failed");
    }
    my_lv_start();

    page_lock_create();
    // solid_test();

    // video_audio_start("/sdcard/am_nr");
}