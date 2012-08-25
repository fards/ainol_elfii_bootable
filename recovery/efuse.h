#ifndef __EFUSE_H_
#define __EFUSE_H_

#include <linux/ioctl.h>

#define EFUSE_ENCRYPT_DISABLE   _IO('f', 0x10)
#define EFUSE_ENCRYPT_ENABLE    _IO('f', 0x20)
#define EFUSE_ENCRYPT_RESET     _IO('f', 0x30)
#define EFUSE_INFO_GET			_IO('f', 0x40)

#define MAX_EFUSE_BYTES         	512

#define EFUSE_NONE_ID			0
#define EFUSE_VERSION_ID		1
#define EFUSE_LICENCE_ID		2
#define EFUSE_MAC_ID				3
#define EFUSE_MAC_WIFI_ID	4
#define EFUSE_MAC_BT_ID		5
#define EFUSE_HDCP_ID			6
#define EFUSE_USID_ID				7
#define EFUSE_MACHINEID_ID			10  /* add for m6 */

#if 0	/* for m3 */
typedef struct efuseinfo_item{
	char title[40];	
	unsigned id;
	unsigned offset;    // write offset	
	unsigned data_len;		
} efuseinfo_item_t;
#else									/* add for m6 */
typedef struct efuseinfo_item{
	char title[40];	
	unsigned id;
	unsigned offset;    // write offset	
	unsigned enc_len;
	unsigned data_len;	
	int bch_en;
	int bch_reverse;
} efuseinfo_item_t;
#endif

typedef enum {
	EFUSE_NONE = 0,
	EFUSE_LICENCE,
	EFUSE_MAC,
	EFUSE_HDCP,
	EFUSE_MAC_BT,
	EFUSE_MAC_WIFI,
	EFUSE_USID,
	EFUSE_VERSION,
	EFUSE_MACHINEID,	  				/* add for m6 */
	EFUSE_TYPE_MAX,
} efuse_type_t;

static char* efuse_title[EFUSE_TYPE_MAX] = {
	NULL,
	"licence",
	"mac",
	"hdcp",
	"mac_bt",
	"mac_wifi",
	"usid",
	"version",
	"machineid",						/* add for m6 */
};

static unsigned int efuse_id[EFUSE_TYPE_MAX] = {
    EFUSE_NONE_ID,
    EFUSE_LICENCE_ID,
    EFUSE_MAC_ID,
    EFUSE_HDCP_ID,
    EFUSE_MAC_BT_ID,
    EFUSE_MAC_WIFI_ID,
    EFUSE_USID_ID,
    EFUSE_VERSION_ID,
    EFUSE_MACHINEID_ID,					/* add for m6 */
};
    
#define EFUSE_DEVICE_NAME	"/dev/efuse"

#endif

