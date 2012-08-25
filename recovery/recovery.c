/*
 * Copyright (C) 2007 The Android Open Source Project
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
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include "efuse.h"

#define REBOOT_NORMAL                        0
#define REBOOT_SHUTDOWN                      1
#define REBOOT_FACTORY_TEST                  2
#define REBOOT_RECOVERY_AGAIN                3

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "update_patch", required_argument, NULL, 'x' },
  { "reboot_to_factorytest", no_argument, NULL, 'f' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "file_copy_from_partition", required_argument, NULL, 'z' },
  { "copy_custom_files", required_argument, NULL, 'a' }, //add ainuo
  { "file1_copy_from_partition", required_argument, NULL, 'y' },
#ifdef RECOVERY_HAS_MEDIA
  { "wipe_media", no_argument, NULL, 'm' },
#endif /* RECOVERY_HAS_MEDIA */
  { "show_text", no_argument, NULL, 't' },
#ifdef RECOVERY_HAS_EFUSE
  { "set_efuse_version", required_argument, NULL, 'v' },
  { "set_efuse_ethernet_mac", optional_argument, NULL, 'd' },
  { "set_efuse_bluetooth_mac", optional_argument, NULL, 'b' },
  { "set_efuse_machine_id", required_argument, NULL, 'M' },       /* add for m6 */
#ifdef  EFUSE_LICENCE_ENABLE
  { "set_efuse_audio_license", optional_argument, NULL, 'l' },
#endif /* EFUSE_LICENCE_ENABLE */
#endif /* RECOVERY_HAS_EFUSE */
  { NULL, 0, NULL, 0 },
};

static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_LOG_FILE = "/cache/recovery/last_log";
static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *CACHE_ROOT = "/cache";
static const char *SDCARD_ROOT = "/sdcard";
static const char *SDCARD_COMMAND_FILE = "/sdcard/factory_update_param.aml";
static const char *UDISK_ROOT = "/udisk";
static const char *UDISK_COMMAND_FILE = "/udisk/factory_update_param.aml";
#ifdef RECOVERY_HAS_MEDIA
static const char *MEDIA_ROOT = "/media";
#endif /* RECOVERY_HAS_MEDIA */
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *TEMPORARY_INSTALL_FILE = "/tmp/last_install";
static const char *SIDELOAD_TEMP_DIR = "/tmp/sideload";

extern UIParameters ui_parameters;    // from ui.c
#ifdef RECOVERY_HAS_EFUSE
extern int recovery_efuse(int confirm, char* args);
#endif /* RECOVERY_HAS_EFUSE */

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_volume() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a given path, mounting partitions as necessary
FILE*
fopen_path(const char *path, const char *mode) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

	// --- if that doesn't work, try the command file form bootloader:recovery_command
	if (*argc <= 1) {
    char *parg = NULL;
		char *recovery_command = fw_getenv ("recovery_command");
        if (recovery_command != NULL && strcmp(recovery_command, "")) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
			strcpy(buf, recovery_command);
			
			if((parg = strtok(buf, "#")) == NULL){
				LOGE("Bad bootloader arguments\n\"%.20s\"\n", recovery_command);
			}else{
				(*argv)[1] = strdup(parg);  // Strip newline.
				for (*argc = 2; *argc < MAX_ARGS; ++*argc) {
					if((parg = strtok(NULL, "#")) == NULL){
						break;
					}else{
						(*argv)[*argc] = strdup(parg);  // Strip newline.
					}
            	}
            	LOGI("Got arguments from bootloader\n");
			}
            
        } else {
            LOGE("Bad bootloader arguments\n\"%.20s\"\n", recovery_command);
        }
    }
	
    // --- if that doesn't work, try the command file
    char * temp_args =NULL;
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ) {
                if (!fgets(buf, sizeof(buf), fp)) break;
			temp_args = strtok(buf, "\r\n");
			if(temp_args == NULL)  continue;
	       		(*argv)[*argc]  = strdup(temp_args);   // Strip newline.      
                		++*argc;
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // -- sleep 1 second to ensure SD card initialization complete
    usleep(1000000);

    // --- if that doesn't work, try the sdcard command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(SDCARD_COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ) {
                if (!fgets(buf, sizeof(buf), fp)) break;
			temp_args = strtok(buf, "\r\n");
			if(temp_args == NULL)  continue;
	       		(*argv)[*argc]  = strdup(temp_args);   // Strip newline.      
                		++*argc;
            }

            check_and_fclose(fp, SDCARD_COMMAND_FILE);
            LOGI("Got arguments from %s\n", SDCARD_COMMAND_FILE);
        }
    }

    // --- if that doesn't work, try the udisk command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(UDISK_COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ) {
                if (!fgets(buf, sizeof(buf), fp)) break;
			temp_args = strtok(buf, "\r\n");
			if(temp_args == NULL)  continue;
	       		(*argv)[*argc]  = strdup(temp_args);   // Strip newline.      
                		++*argc;
            }

            check_and_fclose(fp, UDISK_COMMAND_FILE);
            LOGI("Got arguments from %s\n", UDISK_COMMAND_FILE);
        }
    }

    // --- if no argument, then force show_text
    if (*argc <= 1) {
        char *argv0 = (*argv)[0];
        *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
        (*argv)[0] = argv0;  // use the same program name
        (*argv)[1] = "--show_text";
        *argc = 2;
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}

static void
set_sdcard_update_bootloader_message() {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// How much of the temp log we have copied to the copy in cache.
static long tmplog_offset = 0;

static void
copy_log_file(const char* source, const char* destination, int append) {
    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        LOGE("Can't open %s\n", destination);
    } else {
        FILE *tmplog = fopen(source, "r");
        if (tmplog != NULL) {
            if (append) {
                fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            if (append) {
                tmplog_offset = ftell(tmplog);
            }
            check_and_fclose(tmplog, source);
        }
        check_and_fclose(log, destination);
    }
}


// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    copy_log_file(TEMPORARY_LOG_FILE, LOG_FILE, true);
    copy_log_file(TEMPORARY_LOG_FILE, LAST_LOG_FILE, false);
    copy_log_file(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE, false);
    chmod(LOG_FILE, 0600);
    chown(LOG_FILE, 1000, 1000);   // system user
    chmod(LAST_LOG_FILE, 0640);
    chmod(LAST_INSTALL_FILE, 0644);

    // Reset to mormal system boot so recovery won't cycle indefinitely.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted(COMMAND_FILE) != 0 ||
        (unlink(COMMAND_FILE) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    ensure_path_unmounted(CACHE_ROOT);
    sync();  // For good measure.
}

static int
erase_volume(const char *volume) {
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s...\n", volume);

    ensure_path_unmounted(volume);

    if (strcmp(volume, "/cache") == 0) {
        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
    }

    return format_volume(volume);
}

static char*
copy_sideloaded_package(const char* original_path) {
  if (ensure_path_mounted(original_path) != 0) {
    LOGE("Can't mount %s\n", original_path);
    return NULL;
  }

  if (ensure_path_mounted(SIDELOAD_TEMP_DIR) != 0) {
    LOGE("Can't mount %s\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }

  if (mkdir(SIDELOAD_TEMP_DIR, 0700) != 0) {
    if (errno != EEXIST) {
      LOGE("Can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
      return NULL;
    }
  }

  // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
  // directory, owned by root, readable and writable only by root.
  struct stat st;
  if (stat(SIDELOAD_TEMP_DIR, &st) != 0) {
    LOGE("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
    return NULL;
  }
  if (!S_ISDIR(st.st_mode)) {
    LOGE("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }
  if ((st.st_mode & 0777) != 0700) {
    LOGE("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
    return NULL;
  }
  if (st.st_uid != 0) {
    LOGE("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR, st.st_uid);
    return NULL;
  }

  char copy_path[PATH_MAX];
  strcpy(copy_path, SIDELOAD_TEMP_DIR);
  strcat(copy_path, "/package.zip");

  char* buffer = malloc(BUFSIZ);
  if (buffer == NULL) {
    LOGE("Failed to allocate buffer\n");
    return NULL;
  }

  size_t read;
  FILE* fin = fopen(original_path, "rb");
  if (fin == NULL) {
    LOGE("Failed to open %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }
  FILE* fout = fopen(copy_path, "wb");
  if (fout == NULL) {
    LOGE("Failed to open %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) {
    if (fwrite(buffer, 1, read, fout) != read) {
      LOGE("Short write of %s (%s)\n", copy_path, strerror(errno));
      return NULL;
    }
  }

  free(buffer);

  if (fclose(fout) != 0) {
    LOGE("Failed to close %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  if (fclose(fin) != 0) {
    LOGE("Failed to close %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }

  // "adb push" is happy to overwrite read-only files when it's
  // running as root, but we'll try anyway.
  if (chmod(copy_path, 0400) != 0) {
    LOGE("Failed to chmod %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  return strdup(copy_path);
}

char**
prepend_title(const char** headers) {
    char* title[] = { "Android system recovery <"
                          EXPAND(RECOVERY_API_VERSION) "e>",
                      "",
                      NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = (char **)headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = (char **)headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

int
get_menu_selection(char** headers, char** items, int menu_only,
                   int initial_selection) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    ui_start_menu(headers, items, initial_selection);
    int selected = initial_selection;
    int chosen_item = -1;

    while (chosen_item < 0) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == -1) {   // ui_wait_key() timed out
            if (ui_text_ever_visible()) {
                continue;
            } else {
                LOGI("timed out waiting for key input; rebooting.\n");
                ui_end_menu();
                return ITEM_REBOOT;
            }
        }

        int action = device_handle_key(key, visible);

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT_ITEM:
                    chosen_item = selected;
                    break;
                case NO_ACTION:
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }
    }

    ui_end_menu();
    return chosen_item;
}

static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int
update_directory(const char* path, const char* unmount_when_done,
                 int* wipe_cache) {
    ensure_path_mounted(unmount_when_done);

    const char* MENU_HEADERS[] = { "Choose a package to install:",
                                   path,
                                   "",
                                   NULL };
    const char* MENU_HEADERS_ROOT[] = { "Choose a package source location:",
                                        "",
                                        NULL };
    DIR* d;
    struct dirent* de;
    unsigned char rootlvl = (strcmp(unmount_when_done, "/") ? 0: 1);
    const char* unmount_when_done_new = unmount_when_done;
    d = opendir(path);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path, strerror(errno));
        if (unmount_when_done_new != NULL) {
            ensure_path_unmounted(unmount_when_done_new);
        }
        return 0;
    }

    char** headers = prepend_title((rootlvl ? MENU_HEADERS_ROOT : MENU_HEADERS));

    int d_size = 0;
    int d_alloc = 10;
    char** dirs = malloc(d_alloc * sizeof(char*));
    int z_size = 1;
    int z_alloc = 10;
    char** zips = malloc(z_alloc * sizeof(char*));
    zips[0] = strdup("../");

    while ((de = readdir(d)) != NULL) {
        int name_len = strlen(de->d_name);

        if (de->d_type == DT_DIR) {
            // skip "." and ".." entries
            if (name_len == 1 && de->d_name[0] == '.') continue;
            if (name_len == 2 && de->d_name[0] == '.' &&
                de->d_name[1] == '.') continue;

            // only display supported mount point in root level
            if (rootlvl) {
                if (strcmp(de->d_name, "sdcard") == 0)
                    unmount_when_done_new = SDCARD_ROOT;
                else if (strcmp(de->d_name, "udisk") == 0)
                    unmount_when_done_new = UDISK_ROOT;
                else
                    continue;
            }

            if (d_size >= d_alloc) {
                d_alloc *= 2;
                dirs = realloc(dirs, d_alloc * sizeof(char*));
            }
            dirs[d_size] = malloc(name_len + 2);
            strcpy(dirs[d_size], de->d_name);
            dirs[d_size][name_len] = '/';
            dirs[d_size][name_len+1] = '\0';
            ++d_size;
        } else if (!rootlvl &&
                   de->d_type == DT_REG &&
                   name_len >= 4 &&
                   strncasecmp(de->d_name + (name_len-4), ".zip", 4) == 0) {
            if (z_size >= z_alloc) {
                z_alloc *= 2;
                zips = realloc(zips, z_alloc * sizeof(char*));
            }
            zips[z_size++] = strdup(de->d_name);
        }
    }
    closedir(d);

    qsort(dirs, d_size, sizeof(char*), compare_string);
    qsort(zips, z_size, sizeof(char*), compare_string);

    // append dirs to the zips list
    if (d_size + z_size + 1 > z_alloc) {
        z_alloc = d_size + z_size + 1;
        zips = realloc(zips, z_alloc * sizeof(char*));
    }
    memcpy(zips + z_size, dirs, d_size * sizeof(char*));
    free(dirs);
    z_size += d_size;
    zips[z_size] = NULL;

    int result;
    int chosen_item = 0;
    do {
        chosen_item = get_menu_selection(headers, zips, 1, chosen_item);

        char* item = zips[chosen_item];
        int item_len = strlen(item);
        if (chosen_item == 0) {          // item 0 is always "../"
            // go up but continue browsing (if the caller is update_directory)
            result = -1;
            break;
        } else if (item[item_len-1] == '/') {
            // recurse down into a subdirectory
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            if (path[strlen(path) - 1] != '/')
                strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);
            new_path[strlen(new_path)-1] = '\0';  // truncate the trailing '/'
            result = update_directory(new_path, unmount_when_done_new, wipe_cache);
            if (result >= 0) break;
        } else {
            // selected a zip file:  attempt to install it, and return
            // the status to the caller.
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            if (path[strlen(path) - 1] != '/')
                strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);

            ui_print("\n-- Install %s ...\n", path);
            set_sdcard_update_bootloader_message();
            char* copy = copy_sideloaded_package(new_path);
            if (unmount_when_done_new != NULL) {
                ensure_path_unmounted(unmount_when_done_new);
            }
            if (copy) {
                result = install_package(copy, wipe_cache, TEMPORARY_INSTALL_FILE);
                free(copy);
            } else {
                result = INSTALL_ERROR;
            }
            break;
        }
    } while (true);

    int i;
    for (i = 0; i < z_size; ++i) free(zips[i]);
    free(zips);
    free(headers);

    if (unmount_when_done_new != NULL) {
        ensure_path_unmounted(unmount_when_done_new);
    }
    return result;
}

static void
wipe_data(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "Confirm wipe of all user data?",
                                "  THIS CAN NOT BE UNDONE.",
                                "",
                                NULL };
            title_headers = prepend_title((const char**)headers);
        }

        char* items[] = { " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " Yes -- delete all user data",   // [7]
                          " No",
                          " No",
                          " No",
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1, 0);
        if (chosen_item != 7) {
            return;
        }
    }

    ui_print("\n-- Wiping data...\n");
    device_wipe_data();
    erase_volume("/data");
    erase_volume("/cache");
    ui_print("Data wipe complete.\n");
}

#ifdef RECOVERY_HAS_MEDIA
static void
wipe_media(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "Confirm wipe of all media data?",
                                "  THIS CAN NOT BE UNDONE.",
                                "",
                                NULL };
            title_headers = prepend_title((const char**)headers);
        }

        char* items[] = { " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " Yes -- delete all media data",   // [7]
                          " No",
                          " No",
                          " No",
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1, 0);
        if (chosen_item != 7) {
            return;
        }
    }

    ui_print("\n-- Wiping media...\n");
    erase_volume(MEDIA_ROOT);
    ui_print("Media wipe complete.\n");
}
#endif /* RECOVERY_HAS_MEDIA */

static int
prompt_and_wait() {
    char** headers = prepend_title((const char**)MENU_HEADERS);

    for (;;) {
        finish_recovery(NULL);
        ui_reset_progress();

        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0, 0);

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        int status;
        int wipe_cache;
        switch (chosen_item) {
            case ITEM_REBOOT:
                return REBOOT_NORMAL;
#ifdef RECOVERY_HAS_FACTORY_TEST
	     case ITEM_FACTORY_TEST:
		 return REBOOT_FACTORY_TEST;
#endif
            case ITEM_WIPE_DATA:
                wipe_data(ui_text_visible());
                if (!ui_text_visible()) return REBOOT_NORMAL;
                break;

            case ITEM_WIPE_CACHE:
                ui_print("\n-- Wiping cache...\n");
                erase_volume("/cache");
                ui_print("Cache wipe complete.\n");
                if (!ui_text_visible()) return REBOOT_NORMAL;
                break;

#ifdef RECOVERY_HAS_MEDIA
            case ITEM_WIPE_MEDIA:
                wipe_media(ui_text_visible());
                if (!ui_text_visible()) return REBOOT_NORMAL;
                break;
#endif /* RECOVERY_HAS_MEDIA */

            case ITEM_APPLY_SDCARD:
#ifdef RECOVERY_HAS_SDCARD_ONLY
                status = update_directory(SDCARD_ROOT, SDCARD_ROOT, &wipe_cache);
#else
                status = update_directory("/", "/", &wipe_cache);
#endif /* RECOVERY_HAS_SDCARD_ONLY */
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui_print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui_print("Cache wipe failed.\n");
                    } else {
                        ui_print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui_set_background(BACKGROUND_ICON_ERROR);
                        ui_print("Installation aborted.\n");
                    } else if (!ui_text_visible()) {
                        return REBOOT_NORMAL;  // reboot if logs aren't visible
                    } else {
#ifdef RECOVERY_HAS_SDCARD_ONLY
                        ui_print("\nInstall from sdcard complete.\n");
#else
                        ui_print("\nInstall complete.\n");
#endif /* RECOVERY_HAS_SDCARD_ONLY */
                    }
                }
                break;
            case ITEM_APPLY_CACHE:
                // Don't unmount cache at the end of this.
                status = update_directory(CACHE_ROOT, NULL, &wipe_cache);
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui_print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui_print("Cache wipe failed.\n");
                    } else {
                        ui_print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui_set_background(BACKGROUND_ICON_ERROR);
                        ui_print("Installation aborted.\n");
                    } else if (!ui_text_visible()) {
                        return REBOOT_NORMAL;  // reboot if logs aren't visible
                    } else {
                        ui_print("\nInstall from cache complete.\n");
                    }
                }
                break;

#ifdef RECOVERY_HAS_EFUSE

            case ITEM_WRITE_EFUSE:
                recovery_efuse(-1, NULL);
                if (!ui_text_visible()) return REBOOT_NORMAL;
                break;
#endif /* RECOVERY_HAS_EFUSE */
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie) {
    printf("%s=%s\n", key, name);
}

//add ainuo
char *dump_full_path(const char* path, const char* name)
{
    int path_len = strlen(path) + strlen(name);
    path_len += 10;
    char *path_full = malloc(path_len);
    if (path_full) {
        memset(path_full, 0, path_len);
        strcpy(path_full, path);
        strcat(path_full, "/");
        strcat(path_full, name);
    }

    return path_full;
}

int copy_file(const char* file_dist, const char* file_src)
{
    int ret = 0;
    // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
    // directory, owned by root, readable and writable only by root.
    struct stat st;
    if (stat(file_src, &st) != 0) {
        LOGE("failed to stat %s (%s)\n", file_src, strerror(errno));
        return -1;
    }
   
    if (!S_ISREG(st.st_mode)) {
        LOGE("%s isn't a regular file\n", file_src);
        return -1;
    }

    char* buffer = malloc(BUFSIZ);
    if (buffer == NULL) {
        LOGE("Failed to allocate buffer\n");
        return -1;
    }

    size_t read;
    FILE* fin = fopen(file_src, "rb");
    if (fin == NULL) {
        LOGE("Failed to open %s (%s)\n", file_src, strerror(errno));
        ret = -1;
        goto out1;
    }
   
    FILE* fout = fopen(file_dist, "wb");
    if (fout == NULL) {
        LOGE("Failed to open %s (%s)\n", file_dist, strerror(errno));
        ret = -1;
        goto out1;
    }

    while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) {
        if (fwrite(buffer, 1, read, fout) != read) {
            LOGE("Short write of %s (%s)\n", file_dist, strerror(errno));
            ret = -1;
            goto out1;
        }
    }
   
out1:
    free(buffer);

    if ((fout != NULL) && fclose(fout) != 0) {
        LOGE("Failed to close %s (%s)\n", file_dist, strerror(errno));
    }

    if ((fin != NULL) && fclose(fin) != 0) {
        LOGE("Failed to close %s (%s)\n", file_src, strerror(errno));
    }

    if (chmod(file_dist, 0666) != 0) {
        LOGE("Failed to chmod %s (%s)\n", file_dist, strerror(errno));
        ret = -1;
    }

    return ret;
}


int copy_dir(const char* path_dist, const char* path_src)
{
    int ret = 0;
    DIR* d;
    struct dirent* de;
    d = opendir(path_src);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path_src, strerror(errno));
        return -1;
    }

    if (mkdir(path_dist, 0666) != 0) {
        if (errno != EEXIST) {
            LOGE("Can't mkdir %s (%s)\n", path_dist, strerror(errno));
            return -1;
        }
    }

    //ui_print("copy %s  to %s \n", path_src, path_dist);

    while ((de = readdir(d)) != NULL) {
        int name_len = strlen(de->d_name);

        if (de->d_type == DT_DIR) {
            // skip "." and ".." entries
            if (name_len == 1 && de->d_name[0] == '.') continue;
            if (name_len == 2 && de->d_name[0] == '.' &&
                de->d_name[1] == '.') continue;

           
            char *path_dist_sub = dump_full_path(path_dist, de->d_name);
            char *path_src_sub = dump_full_path(path_src, de->d_name);
            if (path_dist_sub && path_src_sub) {
                if ( copy_dir(path_dist_sub, path_src_sub) ) {
                    ret = -1;
                }
            }
           
            if (path_dist_sub)
                free(path_dist_sub);
            if (path_src_sub)
                free(path_src_sub);
        } else if (de->d_type == DT_REG ) {
       
            char *file_dist = dump_full_path(path_dist, de->d_name);
            char *file_src = dump_full_path(path_src, de->d_name);
            if (file_dist && file_src) {
                //ui_print("copy %s  to %s \n", file_src, file_dist);
                if (copy_file(file_dist, file_src) < 0) {
                    ret = -1;
                    LOGE("Can't copy %s  to %s \n", file_src, file_dist);
                }
            }

            if (file_dist)
                free(file_dist);
            if (file_src)
                free(file_src);
        }
    }
    closedir(d);

    return ret;
}


int copy_custom_files(const char* path_dist, const char* path_src)
{
    if (ensure_path_mounted(path_dist) != 0) {
        LOGE("Can't mount %s\n", path_dist);
        return INSTALL_ERROR;
    }

    if (ensure_path_mounted(path_src) != 0) {
        LOGE("Can't mount %s\n", path_src);
        return INSTALL_ERROR;
    }

    if (copy_dir(path_dist, path_src) < 0)
        return INSTALL_ERROR;
    else
        return INSTALL_SUCCESS;

   
}

//add ainuo end



int
main(int argc, char **argv) {
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    printf("Starting recovery on %s", ctime(&start));

    device_ui_init(&ui_parameters);
    ui_init();
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    load_volume_table();
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    const char *update_patch = NULL;
	const char *file_copy_from_partition_args = NULL;
	const char *copy_custom_files_dir = NULL; //add ainuo
	const char *file1_copy_from_partition_args = NULL;
    int wipe_data = 0, wipe_cache = 0;
    int reboot_to_factorymode = 0;
#ifdef RECOVERY_HAS_MEDIA
    int wipe_media = 0;
#endif /* RECOVERY_HAS_MEDIA */
#ifdef RECOVERY_HAS_EFUSE
    const char *efuse_version = NULL;
	const char *efuse_machine = NULL;					/* add for m6 */
    int set_efuse_version = 0;
    int set_efuse_ethernet_mac = 0;
    int set_efuse_bluetooth_mac = 0;
	int set_efuse_machine_id = 0;						/* add for m6 */
#ifdef EFUSE_LICENCE_ENABLE
    int set_efuse_audio_license = 0;
#endif /* EFUSE_LICENCE_ENABLE */
#endif /* RECOVERY_HAS_EFUSE */

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'a': copy_custom_files_dir = optarg; break; //add ainuo
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
		case 'x': update_patch = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case 'f': reboot_to_factorymode = 1; break;
		case 'z': file_copy_from_partition_args = optarg; break;
		case 'y': file1_copy_from_partition_args = optarg; break;
#ifdef RECOVERY_HAS_MEDIA
        case 'm': wipe_media = 1; break;
#endif /* RECOVERY_HAS_MEDIA */
        case 't': ui_show_text(1); break;
#ifdef RECOVERY_HAS_EFUSE
        case 'v': set_efuse_version = 1; efuse_version = optarg; break;
        case 'd': set_efuse_ethernet_mac = 1; break;
        case 'b': set_efuse_bluetooth_mac = 1; break;
		case 'M': set_efuse_machine_id = 1; efuse_machine = optarg; break; 	/* add for m6 */
#ifdef EFUSE_LICENCE_ENABLE
        case 'l': set_efuse_audio_license = 1; break;
#endif /* EFUSE_LICENCE_ENABLE */

#endif /* RECOVERY_HAS_EFUSE */
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    device_recovery_start();

    printf("Command:");
    for (arg = 0; arg < argc; arg++) {
        printf(" \"%s\"", argv[arg]);
    }
    printf("\n");

    /**
     *  Disable auto reformat, we should *NOT* do this.
     *
     *  For /media partition, we cannot do it because this will break
     *  any file system that's non-FAT.
     */
#if 0
    if (ensure_path_mounted("/data") != 0) {
        ui_print("Can't mount 'data', wipe it!\n");
        if (erase_volume("/data")) {
            ui_print("Data wipe failed.\n");
        }
    }

    if (ensure_path_mounted("/cache") != 0) {
        ui_print("Can't mount 'cache', wipe it!\n");
        if (erase_volume("/cache")) {
            ui_print("Cache wipe failed.\n");
        }
    }

#ifdef RECOVERY_HAS_MEDIA
    if (ensure_path_mounted(MEDIA_ROOT) != 0) {
        ui_print("Can't mount 'media', wipe it!\n");
        if (erase_volume(MEDIA_ROOT)) {
            ui_print("Media wipe failed.\n");
        }
    }
#endif
#endif /* 0 */

	char *file_path = NULL;
	char *partition_type = NULL;
	char *partition = NULL;
	char *offset_str = NULL;
	char *size_str = NULL;
	ssize_t part_offset;
	ssize_t file_size;

	if(file_copy_from_partition_args)
	{
		if(((file_path = strtok(file_copy_from_partition_args, ":")) == NULL) ||
			((partition_type = strtok(NULL, ":")) == NULL)	||
			((partition = strtok(NULL, ":")) == NULL)	||
			((offset_str = strtok(NULL, ":")) == NULL)	||
			((size_str = strtok(NULL, ":")) == NULL))
		{
			printf("file_copy_from_partition_args Invalid!\n");
		}
		else
		{
			part_offset = atoi(offset_str);
			file_size = atoi(size_str);
			file_copy_from_partition(file_path, partition_type, partition, part_offset, file_size);
		}	
	}

	if(file1_copy_from_partition_args)
	{
		if(((file_path = strtok(file1_copy_from_partition_args, ":")) == NULL) ||
			((partition_type = strtok(NULL, ":")) == NULL)	||
			((partition = strtok(NULL, ":")) == NULL)	||
			((offset_str = strtok(NULL, ":")) == NULL)	||
			((size_str = strtok(NULL, ":")) == NULL))
		{
			printf("file1_copy_from_partition_args Invalid!\n");
		}
		else
		{
			part_offset = atoi(offset_str);
			file_size = atoi(size_str);
			file_copy_from_partition(file_path, partition_type, partition, part_offset, file_size);
		}	
	}

    if (update_package) {
        // For backwards compatibility on the cache partition only, if
        // we're given an old 'root' path "CACHE:foo", change it to
        // "/cache/foo".
        if (strncmp(update_package, "CACHE:", 6) == 0) {
            int len = strlen(update_package) + 10;
            char* modified_path = malloc(len);
            strlcpy(modified_path, "/cache/", len);
            strlcat(modified_path, update_package+6, len);
            printf("(replacing path \"%s\" with \"%s\")\n",
                   update_package, modified_path);
            update_package = modified_path;
        }
    }
    printf("\n");

    property_list(print_property, NULL);
    printf("\n");

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package, &wipe_cache, TEMPORARY_INSTALL_FILE);
        if (status == INSTALL_SUCCESS && wipe_cache) {
            if (erase_volume("/cache")) {
                LOGE("Cache wipe (requested by package) failed.");
            }
        }
        if (status != INSTALL_SUCCESS)
		{
			ui_print("Installation aborted.\n");
			goto process_failed;
        }
    }

    if (update_patch != NULL) {
        status = install_package(update_patch, &wipe_cache, TEMPORARY_INSTALL_FILE);
        if (status != INSTALL_SUCCESS)
		{
			ui_print("Installation patch aborted.\n");
			goto process_failed;
		}
    }
	
    if (wipe_data) {
        if (device_wipe_data()) status = INSTALL_ERROR;
        if (erase_volume("/data")) status = INSTALL_ERROR;
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS)
		{
			ui_print("Data wipe failed.\n");
			goto process_failed;
        }
    }
    
    if (wipe_cache) {
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS)
		{
			ui_print("Cache wipe failed.\n");
			goto process_failed;
        }
    }
    
#ifdef RECOVERY_HAS_MEDIA
    if (wipe_media) {
        if (wipe_media && erase_volume(MEDIA_ROOT)) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS)
		{
			ui_print("Media wipe failed.\n");
			goto process_failed;
        }
    } 
#endif /* RECOVERY_HAS_MEDIA */    
//    else {
//        status = INSTALL_ERROR;  // No command specified
//    }

    //add ainuo
    if (copy_custom_files_dir != NULL) {
        status = copy_custom_files(MEDIA_ROOT, copy_custom_files_dir);
        if (status != INSTALL_SUCCESS) ui_print("Copy custom files failed.\n");
    }

#ifdef RECOVERY_HAS_EFUSE
    if (set_efuse_version) {
        status = recovery_efuse(EFUSE_VERSION, efuse_version);
		if (status != INSTALL_SUCCESS)
		{
			ui_print("efuse write version failed.\n");
			goto process_failed;
        }
    }
#ifdef EFUSE_LICENCE_ENABLE
    if (set_efuse_audio_license) {
        status = recovery_efuse(EFUSE_LICENCE, NULL);
		if (status != INSTALL_SUCCESS)
		{
			ui_print("efuse write licence failed.\n");
			goto process_failed;
        }
    }
#endif /* EFUSE_LICENCE_ENABLE */

    if (set_efuse_ethernet_mac) {
        status = recovery_efuse(EFUSE_MAC, NULL);
		if (status != INSTALL_SUCCESS)
		{
			ui_print("efuse write MAC addr failed.\n");
			goto process_failed;
        }
    }

    if (set_efuse_bluetooth_mac) {
        status = recovery_efuse(EFUSE_MAC_BT, NULL);
		if (status != INSTALL_SUCCESS)
		{
			ui_print("efuse write BT MAC failed.\n");
			goto process_failed;
        }
	}
	
	if (set_efuse_machine_id) {							/* add for m6 */
        status = recovery_efuse(EFUSE_MACHINEID, efuse_machine);
    }	
#endif /* RECOVERY_HAS_EFUSE */

	int howReboot;
process_failed:
    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) {
        ui_show_text(1);
        howReboot = prompt_and_wait();
        if (REBOOT_FACTORY_TEST == howReboot)
            reboot_to_factorymode = 1;
    }

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    ui_print("Rebooting...\n");

    sync();

    if (reboot_to_factorymode) {
        property_set("androidboot.mode", "factorytest");
        android_reboot(ANDROID_RB_RESTART, 0, "factory_testl_reboot");
    } else {
        android_reboot(ANDROID_RB_RESTART, 0, 0);
    }

    return EXIT_SUCCESS;
}
