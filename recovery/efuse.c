/*
 * Copyright (C) 2011 Amlogic Inc
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
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <math.h>

#include <malloc.h>
#include <asm/byteorder.h>
#include <linux/types.h>

#include <cutils/properties.h>
#include <cutils/efuse_bch_8.h>

#include "common.h"
#include "roots.h"
#include "recovery_ui.h"
#include "efuse.h"

#define EFUSE_BYTES	8
#define EFUSE_MACLEN	17

const char *efuse_dev = "/dev/efuse";

static const char *SDCARD_AUDIO_LICENSE = "/sdcard/license.efuse";
static const char *SDCARD_AUDIO_LICENSE_OLD = "/sdcard/licence1.ef";

static const char *SDCARD_ETHERNET_MAC = "/sdcard/ethmac.efuse";
static const char *SDCARD_ETHERNET_MAC_OLD = "/sdcard/mac.ef";

static const char *SDCARD_BLUETOOTH_MAC = "/sdcard/btmac.efuse";
static const char *SDCARD_BLUETOOTH_MAC_OLD = "/sdcard/btmac.ef";



#ifdef EFUSE_LICENCE_ENABLE
#define EFUSE_MENU_MAX 3
#else
#define EFUSE_MENU_MAX 2
#endif /* EFUSE_LICENCE_ENABLE */

const char *efuse_items[EFUSE_MENU_MAX + 1] = {
#ifdef EFUSE_LICENCE_ENABLE
    "audio license", 
#endif /* EFUSE_LICENCE_ENABLE */
    "ethernet mac address",
    "bluetooth mac address", 
    NULL 
};

const int efuse_item_id[EFUSE_MENU_MAX + 1] = {
#ifdef EFUSE_LICENCE_ENABLE
    EFUSE_LICENCE, 
#endif /* EFUSE_LICENCE_ENABLE */
    EFUSE_MAC,
    EFUSE_MAC_BT,
    EFUSE_NONE 
};

extern FILE *fopen_path(const char *path, const char *mode);
extern void check_and_fclose(FILE *fp, const char *name);
extern int get_menu_selection(char** headers, char** items, int menu_only, int initial_selection);
extern char **prepend_title(const char** headers);

static int
efuse_opendev()
{
    int fd = open(efuse_dev, O_RDWR);
    if (fd < 0) 
        LOGE("efuse device not found\n");
    return fd;
}

static void
efuse_closedev(int fd)
{
    close(fd);
}

static size_t efuse_read(int efuse_type, char* result_buffer, size_t buffer_size)
{
	loff_t ppos;
	size_t count;
	size_t read_size = 0;
	int fd = -1;
	efuseinfo_item_t efuseinfo_item;

	memset(&efuseinfo_item, 0, sizeof(efuseinfo_item_t));
	efuseinfo_item.id = efuse_id[efuse_type];
	
	fd  = efuse_opendev();

	if (fd >= 0) {	
		if (ioctl(fd, EFUSE_INFO_GET, &efuseinfo_item))
			goto error;
					
		ppos = efuseinfo_item.offset;
		count = efuseinfo_item.data_len;
	
		if (buffer_size > count) {
			printf("error, buffer size not enough");
			goto error;
		}
		lseek(fd, ppos, SEEK_SET);
		read_size = read(fd, result_buffer, count);
		if(read_size != count)
			goto error;
		
		efuse_closedev(fd);
	}
	
	return read_size;
error:
	ui_print("read efuse data %s error\n", efuse_title[efuse_type]); 
	if(fd >= 0)
	    efuse_closedev(fd);
	return -1 ;
}

static size_t efuse_write(int efuse_type, unsigned char *data, size_t buffer_size)
{
	size_t count;
	int ppos;
	size_t write_size = 0;
	int fd = -1;
	efuseinfo_item_t efuseinfo_item;
    
	memset(&efuseinfo_item, 0, sizeof(efuseinfo_item_t));
	efuseinfo_item.id = efuse_id[efuse_type];
	
	fd  = efuse_opendev();

	if (fd >= 0) {
		if (ioctl(fd, EFUSE_INFO_GET, &efuseinfo_item)) {
		    ui_print("efuse ioctl error\n");
			goto error;
	    }
	
		ppos = efuseinfo_item.offset;
		count = efuseinfo_item.data_len;
		
		ui_print("efuse_write offset=%d, data_len=%d\n", ppos, count);
	
		if (buffer_size != count) {
			ui_print("error, efuse data %s format is wrong\n", efuse_title[efuse_type]); 
			goto error;
		}
		if (lseek(fd, ppos, SEEK_SET) == -1)
			goto error;	
		write_size = write(fd, data, buffer_size);
		if (write_size != buffer_size) {
		    ui_print("error, efuse data %s write size wrong\n", efuse_title[efuse_type]); 
			goto error;	
		}
		efuse_closedev(fd);
	} else
	    ui_print("error,%s open file failed\n", efuse_title[efuse_type]); 
	
	return write_size;
error:
	if(fd >= 0)
	    efuse_closedev(fd);
	return -1 ;
}

static int efuse_written_check(int efuse_type)
{
    loff_t ppos;
	int count;
	size_t read_size = 0;
	int fd = -1, i, rc = 0;
    efuseinfo_item_t efuseinfo_item;
    char buffer[MAX_EFUSE_BYTES];

	memset(&efuseinfo_item, 0, sizeof(efuseinfo_item_t));
	efuseinfo_item.id = efuse_id[efuse_type];
	
	fd  = efuse_opendev();
	if (fd >= 0) {
		if (ioctl(fd, EFUSE_INFO_GET, &efuseinfo_item)) {
		    ui_print("can't get efuse info\n"); 
			goto error;
	    }
	
		ppos = efuseinfo_item.offset;
		count = efuseinfo_item.data_len;

		if (lseek(fd, ppos, SEEK_SET) == -1)
			goto error;	
		
		memset(buffer, 0, MAX_EFUSE_BYTES);
		if(count != read(fd, buffer, count))
			goto error;	
			
		for (i = 0; i < count; i++) {
            if (buffer[i]) {
                rc = 1;
                ui_print("this efuse segment has been written\n");
                break;
            }
        }
		efuse_closedev(fd);
	}
	
	return rc;
error:
	if(fd >= 0)
	    efuse_closedev(fd);
	return -1 ;
}


#if 0	//M3:version is 3 bytes.
static int efuse_write_version(char* version_str)
{
    int rc = -1;
    unsigned char version_data[3];
    int version = -1, mach_id = -1;

    sscanf(version_str, "\"version=%d,mach_id=0x%x\"", &version, &mach_id);
    ui_print("version=%x, mach_id=%x \n", version, mach_id);
    
    if(version == -1 || mach_id == -1)
        return -1;
    
    memset(version_data, 0, 3);
    version_data[0] = version & 0xff;
    version_data[1] = mach_id & 0xff;
    version_data[2] = (mach_id >> 8) & 0xff;
    
    if(3 == efuse_write(EFUSE_VERSION, version_data, 3))
        rc = 0;

    return rc;
}
#else	//M6:version is 1 byte.
/* add for m6 */
static int efuse_write_version(char* version_str)
{
    int rc = -1;
    unsigned char version_data[1];
    int version = -1;

	memset(version_data, 0, 1);
	ui_print("version=%s \n", version_str);
	
	version_data[0] = strtol(version_str, NULL, 16);
	version = version_data[0];

	if(version == -1)
		return -1;
	
    if(1 == efuse_write(EFUSE_VERSION, version_data, 1))
    {
    	ui_print("efuse write version(0x%x)sucess\n", version_data[0]);
        rc = 0;
    }	


	//test efuse read version
	if(rc == 0)
	{
		memset(version_data, 0, 1);
  	 	if(1 == efuse_read(EFUSE_VERSION, version_data, 1))
        	ui_print("test efuse read: version(0x%x)sucess\n", version_data[0]); 
	}


    return rc;
		
}
#endif


/* add for m6 */
//write efuse machied id
static int efuse_write_machine(char *machine_str,int type)
{
    int rc = -1;
    unsigned char machine_data[4];
	int i;
	
    if (efuse_written_check(type)) {
        LOGE("%s written already or something error\n", efuse_title[type]);
        return -1;
   }

	ui_print("machine_id=%s \n", machine_str);
	memset(machine_data, 0, 4);
    strcpy(machine_data, machine_str);
	
	ui_print("========efuse_write========\n");   
	if(4 == efuse_write(EFUSE_MACHINEID, machine_data, 4))
	{
		ui_print("efuse write machine_id sucess,machine_id=");
		for(i=0; i<4; i++)
			ui_print("0x%x ",machine_data[i]);
		ui_print("\n\n");
        rc = 0;
	}	 


	//test efuse read machine_id
	if(rc == 0)
	{
		memset(machine_data, 0, 4);
		ui_print("========efuse_read========\n");   
		if(4 == efuse_read(EFUSE_MACHINEID, machine_data, 4))
    	{
    		ui_print("test efuse read: machine_id sucess,machine_id=");
			for(i=0; i<4; i++)
				ui_print("0x%x ",machine_data[i]);
			ui_print("\n\n");	
		}
	}	


 	return rc;

 }



static int
efuse_write_mac(const char *path, int type)
{
    FILE *fp;
    char *rbuff = NULL, *wbuff = NULL;
    char *line;
    int size, rc, error = 0, offset = 0;
    unsigned char mac[6];

    ui_print("Finding %s...\n", efuse_title[type]);

    fp = fopen_path(path, "r");
    if (fp == NULL) {
        LOGE("no %s found\n", efuse_title[type]);
        return -1;
    }

    if (efuse_written_check(type)) {
        LOGE("%s written already or something error\n", efuse_title[type]);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);

    LOGI("efuse_update_mac() path=%s type=%s size=%d\n", path, efuse_title[type], size);

    ui_print("Reading %s...\n", efuse_title[type]);

    rbuff = malloc(size + 1);
    wbuff = malloc(size + 2);
    if (rbuff == NULL || wbuff == NULL) {
        LOGE("system out of resource\n");
        if (rbuff) free(rbuff);
        if (wbuff) free(wbuff);
        check_and_fclose(fp, path);
        return -1;
    }

    fseek(fp, 0, SEEK_SET);
    if ((int)fread(rbuff, 1, size, fp) != size) {
        LOGE("invalid %s\n", efuse_title[type]);
        if (rbuff) free(rbuff);
        if (wbuff) free(wbuff);
        check_and_fclose(fp, path);
        return -1;
    }

    rc = 0;
    rbuff[size] = '\0';
    check_and_fclose(fp, path);

    line = strtok(rbuff, "\n");
    do {
        if (*line == '$' || (strlen(line) != EFUSE_MACLEN && strlen(line) != EFUSE_MACLEN + 1)) {
            offset += strlen(line) + 1 ;
            LOGI("efuse_update_mac() line=\"%s\" SKIP offset=%d\n", line, offset);
            continue;
        }
        for (rc = 0; rc < EFUSE_MACLEN; rc += 3) {
            if (isxdigit(line[rc]) && isxdigit(line[rc + 1]) && (line[rc + 2] == ':' || line[rc + 2] == '\0' || line[rc + 2] == '\r')) {
                mac[rc / 3] = ((isdigit(line[rc]) ? line[rc] - '0' : toupper(line[rc]) - 'A' + 10) << 4) |
                               (isdigit(line[rc + 1]) ? line[rc + 1] - '0' : toupper(line[rc + 1]) - 'A' + 10);
            }
            else
                break;
        }

        if (rc == EFUSE_MACLEN + 1) {
            LOGI("efuse_update_mac() line=\"%s\" MATCH\n", line);

            ui_print("Writing %s %02x:%02x:%02x:%02x:%02x:%02x\n", efuse_title[type], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            if (6 == efuse_write(type, mac, 6)) {
                fp = fopen_path(path, "r");
                if (fp) {
                    fread(rbuff, 1, size, fp);
                    rbuff[size] = '\0';
                    check_and_fclose(fp, path);

                    fp = fopen_path(path, "w+");
                    if (fp) {
                        line = wbuff;
                        memset(wbuff, 0, size + 2);
                        memcpy(line, rbuff, offset);
                        *(line + offset) = '$';
                        memcpy(line + offset + 1, rbuff + offset, size - offset);
                        offset = fwrite(wbuff, 1, size + 1, fp);
                        LOGI("efuse_update_mac() %s wrote rc=%d size=%d\n", path, offset, size + 2);
                        if (offset != size + 1) {
                            error++;
                            LOGE("error updating %s\n", efuse_title[type]);
                        }
                        check_and_fclose(fp, path);
                    }
                }
            }
            else {
                error++;
                LOGE("efuse write error\n");
            }

            break;
        }
        else {
            offset += strlen(line) + 1;
            LOGI("efuse_update_mac() line=\"%s\" INVALID offset=%d\n", line, offset);
        }
    } while((line = strtok(NULL,"\n")));

    if (rc != EFUSE_MACLEN + 1)
        ui_print("No %s found\n", efuse_title[type]);

    if (rbuff) free(rbuff);
    if (wbuff) free(wbuff);
    return -error;
}

#ifdef EFUSE_LICENCE_ENABLE

static int
efuse_audio_license_decode(char *raw, unsigned char *license)
{
    int i;
    char * curr = raw;

    LOGE("efuse_audio_license_decode() raw=%s\n", raw);
    if (!curr)
        return -1;

    for (*license = 0, i = EFUSE_BYTES; i > 0; i--, curr++) {
        if (*curr == '1')
            *license |= 1 << (i - 1);
    }   
    
    LOGE("efuse_audio_license_decode() license=%x\n", *license);
    return 0;
}

/**
 * There are 4 bytes for licence,now we use byte 1--bit[1:0] for cntl 0-ac3,1-dts.
 */
int
efuse_write_audio_license(char *path)
{
    int fd;
    FILE *fp;
    unsigned char license = 0;
    char raw[32];

    ui_print("Finding %s...\n", efuse_title[EFUSE_LICENCE]);
    fp = fopen_path(path, "r");
    if (fp == NULL) {
        LOGE("no %s found\n", efuse_title[EFUSE_LICENCE]);
        return -1;
    }

    ui_print("Reading %s...\n", efuse_title[EFUSE_LICENCE]);
    fgets(raw, sizeof(raw), fp);
    if (strlen(raw) < EFUSE_BYTES) {
        LOGE("invalid %s\n", efuse_title[EFUSE_LICENCE]);
        check_and_fclose(fp, path);
        return -1;
    }

    check_and_fclose(fp, path);
    if (efuse_audio_license_decode(raw, &license)) {
        LOGE("invalid %s\n", efuse_title[EFUSE_LICENCE]);
        check_and_fclose(fp, path);
        return -1;
    }

    ui_print("Writing %s...\n", efuse_title[EFUSE_LICENCE]);
    fd = efuse_opendev();
    if (fd < 0)
        return -1;

    if (lseek(fd, 0, SEEK_SET) == 0) {
        if (write(fd, &license, sizeof(license)) == 1) {
            if ((license & 0x3) > 0)
                ui_print("Audio license enabled\n");
            else
                ui_print("Audio license wrote\n");
        }
        else {
            LOGE("efuse write error\n");
            efuse_closedev(fd);
            return -1;
        }
    }

    efuse_closedev(fd);
    return 0;
}
#endif /*EFUSE_LICENCE_ENABLE */

/**
 *  Recovery efuse programming UI, current support efuse items:
 *  
 *    Audio license
 *    Ethernet MAC address
 *    Bluetooth MAC address
 */
int
recovery_efuse(int interactive, char* args)
{
    const char* menu[] = { "Choose an efuse item to program:",
                           "",
                           NULL };

    char prop[PROPERTY_VALUE_MAX];
    int result = 0;
    int chosen_item = 0;
    int efuse_item_index = 0;
    char **headers = NULL;

    if (interactive < 0) {
        headers = prepend_title(menu);
        chosen_item = get_menu_selection(headers, (char **)efuse_items, 1, chosen_item);
        efuse_item_index = efuse_item_id[chosen_item];
    }
    else
        efuse_item_index = interactive;

    if (efuse_item_index > EFUSE_NONE && efuse_item_index < EFUSE_TYPE_MAX)
        ui_print("\n-- Program %s...\n", efuse_title[efuse_item_index]);

    switch (efuse_item_index) {
        case EFUSE_VERSION:
             result = efuse_write_version((char *)args);
             break;
#ifdef EFUSE_LICENCE_ENABLE
        case EFUSE_LICENCE:
            result = efuse_write_audio_license((char *)SDCARD_AUDIO_LICENSE);
            break;
#endif /* EFUSE_LICENCE_ENABLE */
        case EFUSE_MAC:
            result = efuse_write_mac((char *)SDCARD_ETHERNET_MAC, efuse_item_index);
            break;
        case EFUSE_MAC_BT:
            result = efuse_write_mac((char *)SDCARD_BLUETOOTH_MAC, efuse_item_index);
            break;	
        case EFUSE_MACHINEID:				/* add for m6 */
            result = efuse_write_machine((char *)args, efuse_item_index);
            break;			
    }

    if (efuse_item_index > EFUSE_NONE && efuse_item_index < EFUSE_TYPE_MAX) {
        if (result)
            ui_print("Failed to write %s\n", efuse_title[efuse_item_index]);
        else
            ui_print("\nWrite %s complete\n", efuse_title[efuse_item_index]);
    }

    if (headers) free(headers);
    return result;
}
