/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"

char* MENU_HEADERS[] = { "Android system recovery utility",
                         "",
                         NULL };

char* MENU_ITEMS[] = { "reboot system now",
#ifdef RECOVERY_HAS_SDCARD_ONLY
                       "apply update from sdcard",
#else
                       "apply update",
#endif /* RECOVERY_HAS_SDCARD_ONLY */
                       "wipe data/factory reset",
                       "wipe cache partition",
                       "apply update from cache",
#ifdef RECOVERY_HAS_MEDIA
                       "wipe media partition",
#endif /* RECOVERY_HAS_MEDIA */
#ifdef RECOVERY_HAS_EFUSE
                       "set efuse item",
#endif /* RECOVERY_HAS_EFUSE */
#ifdef RECOVERY_HAS_FACTORY_TEST
                       "reboot into factory test",
#endif /* RECOVERY_HAS_FACTORY_TEST */
                       NULL };

void device_ui_init(UIParameters* ui_parameters) {
}

typedef struct {
    const char* type;
    int value;
    int key[6];
} KeyMapItem;

static int num_keys = 0;
static KeyMapItem* device_keys = NULL;

typedef struct {
    const char *type;
    int value;
} CtrlInfo;

static CtrlInfo g_ctrlinfo[] = {
    { "select", SELECT_ITEM },
    { "down", HIGHLIGHT_DOWN },
    { "up", HIGHLIGHT_UP },
    { "no_action", NO_ACTION },
    { "mode_switch", MODE_SWITCH },
    { "back_door", BACK_DOOR },
};

#define NUM_CTRLINFO (sizeof(g_ctrlinfo) / sizeof(g_ctrlinfo[0]))

static KeyMapItem g_default_keymap[] = {
    { "select", SELECT_ITEM,    {KEY_ENTER, KEY_TAB,        KEY_BACK,        -1, -1, -1} },
    { "down",   HIGHLIGHT_DOWN, {KEY_DOWN,  KEY_VOLUMEDOWN, KEY_PAGEDOWN,    -1, -1, -1} },
    { "up",     HIGHLIGHT_UP,   {KEY_UP,    KEY_VOLUMEUP,   KEY_PAGEUP,      -1, -1, -1} },
};

#define NUM_DEFAULT_KEY_MAP (sizeof(g_default_keymap) / sizeof(g_default_keymap[0]))

static KeyMapItem g_presupposed_keymap[] = {
    { "select", SELECT_ITEM,    {BTN_MOUSE,                 BTN_LEFT,    -1, -1, -1, -1} },
    { "down",   HIGHLIGHT_DOWN, {VIRTUAL_KEY_MOUSE_DOWN, VIRTUAL_KEY_MOUSE_WHEEL_DOWN,  VIRTUAL_KEY_TOUCH_DOWN, BTN_RIGHT,  -1, -1} },
    { "up",     HIGHLIGHT_UP,   {VIRTUAL_KEY_MOUSE_UP,   VIRTUAL_KEY_MOUSE_WHEEL_UP,    VIRTUAL_KEY_TOUCH_UP,    -1,    -1, -1} },
};

#define NUM_PRESUPPOSED_KEY_MAP (sizeof(g_presupposed_keymap) / sizeof(g_presupposed_keymap[0]))

int getKey(char *key){
    unsigned int i;
    for (i = 0; i < NUM_CTRLINFO; i++) {
        CtrlInfo *info = &g_ctrlinfo[i];
        if (strcmp(info->type, key) == 0) {
            return info->value;
        }
    }
    return NO_ACTION;
}

void load_key_map() {
    FILE* fstab = fopen("/etc/recovery.kl", "r");
    if (fstab != NULL) {
        LOGI("loaded /etc/recovery.kl\n");
        int alloc = 2;
        device_keys = malloc(alloc * sizeof(KeyMapItem));

        device_keys[0].type = "select";
        device_keys[0].value = NO_ACTION;
        device_keys[0].key[0] = -1;
        device_keys[0].key[1] = -1;
        device_keys[0].key[2] = -1;
        device_keys[0].key[3] = -1;
        device_keys[0].key[4] = -1;
        device_keys[0].key[5] = -1;
        num_keys = 0;

        char buffer[1024];
        int i;
        while (fgets(buffer, sizeof(buffer)-1, fstab)) {
            for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
            if (buffer[i] == '\0' || buffer[i] == '#') continue;

            char* original = strdup(buffer);

            char* type = strtok(buffer+i, " \t\n");
            char* key1 = strtok(NULL, " \t\n");
            char* key2 = strtok(NULL, " \t\n");
            char* key3 = strtok(NULL, " \t\n");
            char* key4 = strtok(NULL, " \t\n");
            char* key5 = strtok(NULL, " \t\n");
            char* key6 = strtok(NULL, " \t\n");

            if (type && key1) {
                while (num_keys >= alloc) {
                    alloc *= 2;
                    device_keys = realloc(device_keys, alloc*sizeof(KeyMapItem));
                }
                device_keys[num_keys].type = strdup(type);
                device_keys[num_keys].value = getKey(type);
                device_keys[num_keys].key[0] = key1?atoi(key1):-1;
                device_keys[num_keys].key[1] = key2?atoi(key2):-1;
                device_keys[num_keys].key[2] = key3?atoi(key3):-1;
                device_keys[num_keys].key[3] = key4?atoi(key4):-1;
                device_keys[num_keys].key[4] = key5?atoi(key5):-1;
                device_keys[num_keys].key[5] = key6?atoi(key6):-1;

                ++num_keys;
            } else {
                LOGE("skipping malformed recovery.lk line: %s\n", original);
            }
            free(original);
        }

        fclose(fstab);

    } else {
        LOGE("failed to open /etc/recovery.kl, use default map\n");
        num_keys = NUM_DEFAULT_KEY_MAP;
        device_keys = g_default_keymap;
    }

    LOGI("recovery key map table\n");
    LOGI("=========================\n");

    int i;
    for (i = 0; i < num_keys; ++i) {
        KeyMapItem* v = &device_keys[i];
        LOGI("  %d type:%s value:%d key:%d %d %d %d %d %d\n", i, v->type, v->value,
               v->key[0], v->key[1], v->key[2], v->key[3], v->key[4], v->key[5]);
    }
    LOGI("\n");
}

int device_recovery_start() {
    load_key_map();
    return 0;
}

int device_toggle_display(volatile char* key_pressed, int key_code) {
#ifdef RECOVERY_TOGGLE_DISPLAY
    return key_code == KEY_HOME;
#else
    return 0;
#endif /* RECOVERY_TOGGLE_DISPLAY */
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    return 0;
}

int device_handle_key(int key_code, int visible) {
    if (visible) {
        int i,j;
        for (i = 0; i < num_keys; i++) {
            for (j = 0; j < 6; j++) {
            KeyMapItem* v = &device_keys[i];
                if(v->key[j] == key_code)
                    return v->value;
            }
        }

        for (i = 0; i < (int)NUM_PRESUPPOSED_KEY_MAP; i++) {
            for (j = 0; j < 6; j++) {
                if(g_presupposed_keymap[i].key[j] == key_code)
                    return g_presupposed_keymap[i].value;
            }
        }
    }

    return NO_ACTION;
}

int device_perform_action(int which) {
    return which;
}

int device_wipe_data() {
    return 0;
}

