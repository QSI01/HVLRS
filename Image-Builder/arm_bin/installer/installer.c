#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include <unistd.h>
#include <crypt.h>
#include <errno.h>
#include "rtk_common.h"
#include "limits.h"
#include "fwtbl.h"
#include "rtk_config.h"
#include "factory.h"
#include "tar.h"

#define BLKRRPART  _IO(0x12,95)

#define DIGEST_FUNC "sha256sum"

/* MAX_PART_LEN = bootpart(1) + normalpart(3) */
#define MAX_PART_LEN 4

typedef struct fw_mapping_s {
	fw_type_code_e code;
	const char *fw_name;
	char file[256];
} fw_mapping_t;

typedef struct fs_mapping_s {
	fs_type_code_e code;
	char fs_name[32];
	char file[256];
	uint32_t zone_size;
	uint32_t actual_size;
	uint64_t address;
} fs_mapping_t;

static fw_mapping_t fw_support[]= {
	// LABEL, firmware_name, filename, target_address
	{FW_TYPE_KERNEL, 		"linuxKernel"},
	{FW_TYPE_KERNEL_DT, 	"kernelDT"},
	{FW_TYPE_RESCUE_DT, 	"rescueDT"},
	{FW_TYPE_RESCUE_ROOTFS, "rescueRootFS"},
	{FW_TYPE_AUDIO, 		"audioKernel"},
	{FW_TYPE_IMAGE_FILE,	"bootLogo"},
	{FW_TYPE_KERNEL_ROOTFS,	"initramfs"},
};
/* count_part: bootpart + normalpart
 * count_normalpart: normalpart
 */
uint32_t count_part=0, count_normalpart=0;
static fs_mapping_t bootpart, normalpart[MAX_PART_LEN-1];

long int addr_1stfwtbl;
long int addr_2ndfwtbl;
long int fwtbl_size;
unsigned int mtd_erasesize;

char in_factorytar_file[IN_FACTORYTAR_NAMESIZE]={0};
char storage_dev[64]={0};
char *parted_cmd[MAX_PART_LEN+1]={0};
int parted_cmd_len=0;

create_var_fun(debug_level)
create_yn_fun(use_spi)
create_yn_fun(use_nand)
create_yn_fun(use_emmc)
create_var_fun(align_size)
create_var_fun(total_size)

create_yn_fun(update_1stfw)
create_yn_fun(update_2ndfw)

create_var_fun(seqnum_1stfw)
create_var_fun(seqnum_2ndfw)

create_yn_fun(burn_factory)
create_var_fun(factory_start_addr)
create_var_fun(factory_zone)
create_yn_fun(burn_kernel)
create_yn_fun(burn_kerneldtb)
create_yn_fun(burn_rescuedtb)
create_yn_fun(burn_rescue_rootfs)
create_yn_fun(burn_bluecore)
create_yn_fun(burn_initramfs)
create_yn_fun(burn_bootpart)
create_yn_fun(burn_bootlogo)
create_yn_fun(burn_fwtbl)

const char USAGE[] = \
"Usage:" \
" ./install install.img\r\n" \
"\r\n" ;

#define CMD_LEN 512
#define run(x...) \
	do { \
		char cmd[CMD_LEN]; \
		memset(cmd, 0, CMD_LEN); \
		snprintf(cmd, CMD_LEN, x); \
		install_log("Run... \n\"%s\"\n",cmd); \
		if (!(debug_level&INSTALL_FAKE_RUN)) \
			if (system(cmd)!=0) \
				install_fail("fail to execute \n%s\n",cmd); \
	} while(0) 

#define untar_config_txt(x...) \
	do { \
		char cmd[256]; \
		memset(cmd, 0, 256); \
		snprintf(cmd, 256, x); \
		install_log("Run... \n\"%s\"\n",cmd); \
		if (system(cmd)!=0) \
			install_fail("fail to execute \n%s\n",cmd); \
	} while(0) 

#define FWTBL_ZONE_SIZE  0x10000
#define SHA256_SIZE	32

void dump_rawdata(uint8_t *p, uint32_t len)
{
	if (!(debug_level&INSTALL_LOG_LEVEL))
		return;	
	uint32_t i;
	for(i=0; i<len; i++) {
		install_ui("%02x ", p[i]);
		if(0==(i+1)%16) {
			install_ui("\r\n");
		}

	}
	install_ui("\r\n");
}

void dump_fw_desc_table_v1(fw_desc_table_v1_t *p)
{
	if (!(debug_level&INSTALL_LOG_LEVEL))
		return;
	
	if (p != NULL) {
		install_ui("[%s] Firmware Table\r\n", __func__);
		install_ui("%16s: %c%c%c%c%c%c\r\n", "signature", 
		p->signature[0], p->signature[1], p->signature[2], 
		p->signature[3], p->signature[4], p->signature[5]);
		install_ui("%16s: %02x\r\n", "checksum", p->checksum);
		install_ui("%16s: %02x\r\n", "version", p->version);
		install_ui("%16s: %02x\r\n", "paddings", p->paddings);
		install_ui("%16s: %02x\r\n", "part_list_len", p->part_list_len);
		install_ui("%16s: %02x\r\n", "fw_list_len", p->fw_list_len);
		install_ui("%16s: %02x\r\n", "seqnum", p->seqnum);
	}
	else {
		install_ui("[ERR] %s:%d fw_desc_table_v1 is NULL.\n", __FUNCTION__, __LINE__);
	}
	install_ui("raw dump:\r\n");
	dump_rawdata((uint8_t *)p, sizeof(fw_desc_table_v1_t));	
}


void dump_part_desc_entry_v1(part_desc_entry_v1_t *p)
{
	if (p != NULL) {
		install_ui("[%s] Frimware Partition\r\n", __func__);
		install_ui("%16s: %02x\r\n", "type", p->type);
		install_ui("%16s: %02x\r\n", "ro", p->ro);
		install_ui("%16s: %08x\r\n", "length", p->length);
		install_ui("%16s: %08x\r\n", "fw_count", p->fw_count);
		install_ui("%16s: %08x\r\n", "fw_type", p->fw_type);
		install_ui("%16s: %08x\r\n", "partIdx", p->partIdx);		
		install_ui("%16s: %s\r\n", "mount_point", p->mount_point);
	}
	else {
		install_fail("[ERR] %s:%d part_entry is NULL.\n", __FUNCTION__, __LINE__);
	}
	install_ui("raw dump:\r\n");
	dump_rawdata((uint8_t *)p, sizeof(part_desc_entry_v1_t));	
}



void dump_fw_desc_entry_v2(fw_desc_entry_v2_t *p)
{
	uint32_t i;

	install_ui("[%s] Firmwares\r\n", __func__);
	install_ui("%16s: %02x\r\n", "type", p->type);
	install_ui("%16s: %02x\r\n", "reserved", p->reserved);
	install_ui("%16s: %02x\r\n", "lzma", p->lzma);
	install_ui("%16s: %02x\r\n", "ro", p->ro);
	install_ui("%16s: %08x\r\n", "version", p->version);
	install_ui("%16s: %08x\r\n", "target_addr", p->target_addr);
	install_ui("%16s: %llx\r\n", "offset", p->offset);
	install_ui("%16s: %08x\r\n", "length", p->length);
	install_ui("%16s: %08x\r\n", "paddings", p->paddings);

	install_ui("%16s: ", "sha256_hash");
	for (i = 0; i < 32; i++) {
		install_ui("%02x ", p->sha_hash[i]);
	};
	install_ui("\r\n");

	install_ui("%16s: ", "reserved_1");
	for (i = 0; i < 6; i++) {
		install_ui("%02x ", p->reserved_1[i]);
	};
	install_ui("\r\n");

	install_ui("raw dump:\r\n");
	dump_rawdata((uint8_t *)p, sizeof(fw_desc_entry_v2_t));
	return;
}

uint32_t get_checksum(uint8_t *p, uint32_t len)
{
    uint32_t checksum = 0;
    uint32_t i;

    for(i = 0; i < len; i++)
    {
        checksum += *(p+i);
    }
    return checksum;
}

uint32_t prn_get_checksum(uint8_t *p, uint32_t len)
{
    uint32_t checksum = 0;
    uint32_t i;

    for(i = 0; i < len; i++)
    {
        checksum += *(p+i);
		if ((i%16)==15)
			install_ui("\n");
		install_ui("0x%x ", *(p+i));
    }
	install_ui("\n");    
    return checksum;
}

uint8_t ascii_2_hex(char ascii)
{
	switch (ascii)
	{
		case '0' ... '9':
			return (ascii&0x0f);
		case 'a':
		case 'A':
			return 0x0a;
		case 'b':
		case 'B':
			return 0x0b;
		case 'c':
		case 'C':
			return 0x0c;
		case 'd':
		case 'D':
			return 0x0d;
		case 'e':
		case 'E':
			return 0x0e;
		case 'f':
		case 'F':
			return 0x0f;
		default:
			return 0xdb;
	}
}

static void asciis_2_hexs(char *dst, char *src, uint32_t src_size)
{
	uint32_t i;
	for (i=0;i<src_size/2;i++)
		dst[i]=(ascii_2_hex(src[2*i])<<4) | ascii_2_hex(src[2*i+1]);
}	

static int search_fw_typecode(char *fw_name, char *file)
{
	int i;
	install_info("searching typecode of %s for %s\n", fw_name, file);

	for (i=0;i<sizeof(fw_support)/sizeof(fw_mapping_t);i++)
	{
		if (strcmp(fw_support[i].fw_name, fw_name)==0)
		{
			memset(fw_support[i].file, 0,256);
			strcpy(fw_support[i].file, file);
			install_log("Typecode of %s(%s) is %d\n", fw_name, file, fw_support[i].code);
			return fw_support[i].code;
		}
	}
	install_fail("unknown typecode %s\n the supporting type are ...\n", fw_name);
	for (i=0;i<sizeof(fw_support)/sizeof(fw_mapping_t);i++)
	{
		install_fail("%s = %d\n", fw_support[i].fw_name, fw_support[i].code);
	}
	exit(1);
	return FW_TYPE_UNKNOWN;
}

static char* search_fw_filename(int type)
{
	int i;
	for (i=0;i<sizeof(fw_support)/sizeof(fw_mapping_t);i++)
	{
		if (fw_support[i].code==type)
		{
			//printf("found file %d %s\n",type,fw_support[i].file);
			return fw_support[i].file;
		}
	}
	return NULL;
}

static int search_fs_typecode(char *fs_name)
{
	install_info("searching typecode of %s\n", fs_name);
	
	if (strcmp("squashfs", fs_name)==0)	
		return FS_TYPE_SQUASH;

	if (strcmp("swap", fs_name)==0)	
		return FS_TYPE_RAWFILE;

	if (strcmp("raw", fs_name)==0)	
		return FS_TYPE_RAWFILE;

	if (strcmp("ext4", fs_name)==0)	
		return FS_TYPE_EXT4;

	if (strcmp("ubifs", fs_name)==0)	
		return FS_TYPE_UBIFS;

	if (strcmp("overlay", fs_name)==0)
		return FS_TYPE_NONE;

	install_fail("unknown typecode %s\n The supporting type are [squashfs|swap|raw|ext4|ubifs|overlay]\n", fs_name);
	exit(1);

	return FS_TYPE_UNKNOWN;
}

int compute_filesum(const char *filepath, char *hash)
{
	char command[128] = { 0 };
	FILE *fp;

	sprintf(command, "%s %s", DIGEST_FUNC, filepath);
	fp = popen(command, "r");
	if (fgets(hash, 65, fp) == NULL) {	// read in at most 65 - 1 characters
		pclose(fp);
		return -1;
	}

	pclose(fp);
	return 0;
}

static int add_firmware(char* str, fw_desc_entry_v2_t *fwdesc)
{
	uint32_t actual_size, ram_addr, zone_size;
	uint64_t storage_addr;
	char filename[128], compress[128], sha256[65],typecode[128];
	char filepath[256] = { 0 };
	// sanity-check
	if(str == NULL||*str==0)
		return -1;

	if(str[0] == ' ')
		str = skip_space(str);
	install_info("input %s %p\n", str, fwdesc);

	sscanf(str, "%s %s %s %x %llx %x %x", typecode, filename, compress, &ram_addr, &storage_addr, &actual_size, &zone_size);
	install_info("typecode %s\n", typecode);
	install_info("filename %s\n", filename);
	install_info("compress %s\n", compress);
	install_info("ram_addr 0x%x\n", ram_addr);
	install_info("storage_addr 0x%llx\n", storage_addr);
	install_info("actual_size 0x%x\n", actual_size);
	install_info("zone_size 0x%x\n", zone_size);

	/* Compute checksum of file */
	sprintf(filepath, "/tmp/%s", &filename[1]);
	if (compute_filesum(filepath, sha256) != 0)
		return -1;
	install_info("checksum %s\n", sha256);

	fwdesc->type=search_fw_typecode(typecode, &filename[1]); //workaround for sscanf always read a \0 in the first byte of filename

	if (!strcmp(compress, "lzma"))
		fwdesc->lzma=1;

	fwdesc->ro=1;
	fwdesc->version=0;
	fwdesc->target_addr=ram_addr;
	fwdesc->paddings=zone_size;
	fwdesc->offset=storage_addr;
	fwdesc->length=actual_size;
	asciis_2_hexs((char*)fwdesc->sha_hash, sha256, sizeof(sha256));
	dump_fw_desc_entry_v2(fwdesc);
	return 0;
}

int add_partition(char* str, part_desc_entry_v1_t *partdesc, fs_mapping_t *partinfo)
{
	uint32_t actual_size, zone_size;
	uint64_t storage_addr;
	char typecode[32], filename[128];

	// sanity-check
	if(str == NULL||*str==0)
		return -1;
	//printf("newline:%s", str);
	if(str[0] == ' ')
		str = skip_space(str);
	install_info("input %s %p\n", str, partdesc[0]);
	sscanf(str, "%s %s %llx %x %x", typecode, filename, &storage_addr, &actual_size, &zone_size);
	install_info("typecode %s\n", typecode);
	install_info("filename %s\n", filename);
	install_info("storage_addr 0x%llx\n", storage_addr);
	install_info("actual_size 0x%x\n", actual_size);
	install_info("zone_size 0x%x\n", zone_size);
	partdesc[0].fw_type=search_fs_typecode(typecode);
	partdesc[0].type=0;
	partdesc[0].ro=0;
	partdesc[0].length=actual_size;
	partdesc[0].fw_count=1;
	partdesc[0].partIdx=1;//mmcblk0;
	if (partinfo == &bootpart)
		strcpy(partdesc[0].mount_point, "/");
	else
		partdesc[0].mount_point[0] = '\0';
	strcpy(partinfo->file, &filename[1]);
	strcpy(partinfo->fs_name, typecode);
	partinfo->zone_size=zone_size;
	partinfo->actual_size=actual_size;
	partinfo->address=storage_addr;

	// partitions for NAND are specified via kernelargs (set count_part=0 later)
	if (use_nand)
		install_info("NOTE: Partition (part_desc_entry) will not be set in Firmware Table!\n");
	dump_part_desc_entry_v1(partdesc);
	return 0;	
}

int add_factory(char* str) {
	// sanity-check
	char tmp_filename[IN_FACTORYTAR_NAMESIZE];
	if(str == NULL || *str==0)
		return -1;
	//printf("newline:%s", str);
	if(str[0] == ' ')
		str = skip_space(str);

	sscanf(str, "%s", tmp_filename);
	sprintf(in_factorytar_file, "%s", &tmp_filename[1]);
	install_info("in_factorytar_file %s\n", in_factorytar_file);

	return 0;
}

const char signature[8]="VERONA__"; /* 8 bytes signature. */

int fill_fwtbl(fw_desc_table_v1_t* pFWTblDesc, 
	uint32_t fw_count, fw_desc_entry_v2_t *pFWDesc, int seq_num,
	part_desc_entry_v1_t *pPartDesc)
{
	int bytecnt=0;
	int i;

	install_info("Input:Firmware table %p\n", pFWTblDesc);
	install_info("Input:Firmware %p(count %d)\n", pFWDesc, fw_count);
	install_info("Input:Partition %p(count %d)\n", pPartDesc, count_part);
	
	pFWTblDesc->fw_list_len=sizeof(fw_desc_entry_v2_t)*fw_count;
	pFWTblDesc->part_list_len = sizeof(part_desc_entry_v1_t)*count_part;
	// fw_desc setting, checksum fw_desc_entry
	i=0;
	while(i<fw_count)
	{
		pFWTblDesc->checksum += get_checksum((uint8_t*) &pFWDesc[i], sizeof(fw_desc_entry_v2_t));
		bytecnt += sizeof(fw_desc_entry_v2_t);
		i++;
	}
	// part_desc setting, checksum part_desc_entry
	i=0;
	while(i<count_part)
	{
		pFWTblDesc->checksum += get_checksum((uint8_t*) &pPartDesc[i], sizeof(part_desc_entry_v1_t));
		bytecnt += sizeof(part_desc_entry_v1_t);
		i++;
	}
/* 
	"paddings" is not real paddings. It controls how many bytes the bootcode reads.
	padding is also calculated into checksum, so we pre-add the bytecnt and give a paddings value
*/
	pFWTblDesc->version = FW_DESC_TABLE_V2_T_VERSION_2;
	strncpy((char*)pFWTblDesc->signature, signature, 8);
	bytecnt += (sizeof(fw_desc_table_v1_t)-12);
	if (!use_spi && use_nand && !use_emmc)
		pFWTblDesc->paddings = SIZE_ALIGN_BOUNDARY_MORE(bytecnt, mtd_erasesize);
	else
		pFWTblDesc->paddings = SIZE_ALIGN_BOUNDARY_MORE(bytecnt, align_size);
	pFWTblDesc->seqnum = seq_num;
	pFWTblDesc->checksum += get_checksum((uint8_t*)pFWTblDesc + 12, sizeof(fw_desc_table_v1_t)-12);
	dump_fw_desc_table_v1(pFWTblDesc);

	return 0;
}


static void run_burn_spi(uint32_t count_1stfw, fw_desc_entry_v2_t *p1stFWDesc, uint32_t count_2ndfw, fw_desc_entry_v2_t *p2ndFWDesc)
{
	uint32_t is_bootlogo_burn_once = 0;
	install_log("Burning SPI\n");
	install_log("Burning Firmware Table [Y/N]: %c\n",burn_fwtbl?'Y':'N');
	install_log("Burning 1st Firmware Table [Y/N]: %c\n",update_1stfw?'Y':'N');	
	install_log("Burning 2nd Firmware Table [Y/N]: %c\n",update_2ndfw?'Y':'N');	
	install_log("Burning Kernel [Y/N]: %c\n",burn_kernel?'Y':'N');
	install_log("Burning Kernel DTB [Y/N]: %c\n",burn_kerneldtb?'Y':'N');
	install_log("Burning Rescue DTB [Y/N]: %c\n",burn_rescuedtb?'Y':'N');
	install_log("Burning Rescue Rootfs [Y/N]: %c\n",burn_rescue_rootfs?'Y':'N');
	install_log("Burning Bluecore [Y/N]: %c\n",burn_bluecore?'Y':'N');	
	install_log("Burning Factory [Y/N]: %c\n",burn_factory?'Y':'N');
	install_log("Burning BootLogo [Y/N]: %c\n",burn_bootlogo?'Y':'N');
	install_log("Burning initramfs [Y/N]: %c\n",burn_initramfs?'Y':'N');

	if (burn_factory) {
		install_log("Installing Factory...\n");
		install_factory();
	}

	if (burn_fwtbl)
	{
		if ((update_1stfw == 1) && (update_2ndfw == 1)) {
			uint32_t i;
			uint64_t storage_addr_1stfw_bootlogo = 0;
			uint64_t storage_addr_2ndfw_bootlogo = 0;
			for (i = 0; i < count_1stfw; i++) {
				if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_1stfw_bootlogo = p1stFWDesc[i].offset;
				}
			}
			for (i = 0; i < count_2ndfw; i++) {
				if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_2ndfw_bootlogo = p2ndFWDesc[i].offset;
				}
			}
			if ( (storage_addr_1stfw_bootlogo != 0) && (storage_addr_2ndfw_bootlogo != 0) ) {
				if ( storage_addr_1stfw_bootlogo == storage_addr_2ndfw_bootlogo ) {
					is_bootlogo_burn_once=1;
				}
			}
		}
		if (update_1stfw)
		{
			install_log("Erasing 1st Firmware Table...\n");
			run("/tmp/flash_erase /dev/mtd0 0x%lx %ld", addr_1stfwtbl, fwtbl_size/align_size);
			install_log("Writing 1st Firmware Table...\n");
			run("dd if=/tmp/tbl_1stfw.bin of=/dev/mtd0 obs=%ld seek=%ld", align_size, addr_1stfwtbl/align_size);
			install_log("Burning 1st Firmware, count %d%s\n", count_1stfw, !count_1stfw? ", exit!":"");
			if (count_1stfw != 0)
			{
				uint32_t i;
				char *file;

				for(i=0; i<count_1stfw; i++)
				{
					if ((file=search_fw_filename(p1stFWDesc[i].type))==NULL)
					{
						install_fail("typecode %d not found\n",p1stFWDesc[i].type);
						exit(1);
					}
					
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL_ROOTFS)&&(!burn_initramfs))
						continue;

					install_log("[1st FW] Erasing for %s\n", file);
					run("/tmp/flash_erase /dev/mtd0 0x%llx %ld", p1stFWDesc[i].offset, p1stFWDesc[i].paddings/align_size);
					install_log("[1st FW] Writing for %s\n", file);
					run("dd if=/tmp/%s of=/dev/mtd0 obs=%ld seek=%lld", file, align_size, p1stFWDesc[i].offset/align_size);
				}
			}
	
		}
		if (update_2ndfw)
		{
			install_log("Erasing 2nd Firmware Table...\n");
			run("/tmp/flash_erase /dev/mtd0 0x%lx %ld", addr_2ndfwtbl, fwtbl_size/align_size);
			install_log("Writing 2nd Firmware Table...\n");
			run("dd if=/tmp/tbl_2ndfw.bin of=/dev/mtd0 obs=%ld seek=%ld", align_size, addr_2ndfwtbl/align_size);
			install_log("Burning 2nd Firmware, count %d%s\n", count_2ndfw, !count_2ndfw? ", exit!":"");
			if (count_2ndfw != 0)
			{
				uint32_t i;
				char *file;

				for(i=0; i<count_2ndfw; i++)
				{
					if ((file=search_fw_filename(p2ndFWDesc[i].type))==NULL)
					{
						printf("typecode %d not found\n",p2ndFWDesc[i].type);
						exit(1);
					}
					
					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(is_bootlogo_burn_once)) {
						install_debug("[2nd FW] The BootLogo in 1st and 2nd fw table"
									  "are the same, do nothing\n");
						continue;
					}

					install_log("[2nd FW] Erasing for %s\n", file);
					run("/tmp/flash_erase /dev/mtd0 0x%llx %ld", p2ndFWDesc[i].offset, p2ndFWDesc[i].paddings/align_size);
					install_log("[2nd FW] Writing for %s\n", file);
					run("dd if=/tmp/%s of=/dev/mtd0 obs=%ld seek=%lld", file, align_size, p2ndFWDesc[i].offset/align_size);
				}
			}
		}
	}
}

static void prepare_emmc_MBR_for_parted()
{
	#define CMD_MAXLEN 512
	char *cmd;
	char *parted="/tmp/parted -s";
	int i;

	if ((cmd = malloc(CMD_MAXLEN)) == NULL) {
		install_fail("Malloc for parted CMD failed");
		return;
	}
	/* Create MBR type partition table */
	snprintf(cmd, CMD_MAXLEN, "%s %s mklabel msdos", parted, storage_dev);
	parted_cmd[parted_cmd_len++] = cmd;

	if ((cmd = malloc(CMD_MAXLEN)) == NULL) {
		install_fail("Malloc for parted CMD failed");
		return;
	}

	/* Create bootpart partition */
	snprintf(cmd, CMD_MAXLEN, "%s %s unit B mkpart primary %llu %llu"
		,parted
		,storage_dev
		,bootpart.address
		,bootpart.address+bootpart.zone_size-1);
	parted_cmd[parted_cmd_len++] = cmd;

	for (i = 0; i < count_normalpart; i++) {

		if ((cmd = malloc(CMD_MAXLEN)) == NULL) {
			install_fail("Malloc for parted CMD failed");
			return;
		}

		/* Create normalpart partition */
		snprintf(cmd, CMD_MAXLEN, "%s %s unit B mkpart primary %llu %llu"
			,parted
			,storage_dev
			,normalpart[i].address
			,normalpart[i].address+normalpart[i].zone_size-1);
		parted_cmd[parted_cmd_len++] = cmd;
	}
}

static void re_read_partition_table(char *dev)
{
	int dev_fd, ret;

	dev_fd = open(dev, O_RDWR);
	if (dev_fd < 0) {
		install_fail("Fail to open %s: %s\n", dev, strerror(errno));
		return;
	}

	ret = ioctl(dev_fd, BLKRRPART);
	if (ret < 0)
		install_fail("Failed to re-read partition table: %s\n", strerror(errno));

	close(dev_fd);

	return;
}

static void run_burn_emmc(uint32_t count_1stfw, fw_desc_entry_v2_t *p1stFWDesc, uint32_t count_2ndfw, fw_desc_entry_v2_t *p2ndFWDesc, part_desc_entry_v1_t *pPartDesc)
{	
	int burn_partition_table = burn_bootpart | (count_normalpart != 0);
	uint32_t is_bootlogo_burn_once = 0;
	uint32_t i;
	install_log("Burning eMMC\n");
	install_log("Burning Firmware Table [Y/N]: %c\n",burn_fwtbl?'Y':'N');
	install_log("Burning 1st Firmware Table [Y/N]: %c\n",update_1stfw?'Y':'N');	
	install_log("Burning 2nd Firmware Table [Y/N]: %c\n",update_2ndfw?'Y':'N');	
	install_log("Burning Kernel [Y/N]: %c\n",burn_kernel?'Y':'N');
	install_log("Burning Kernel DTB [Y/N]: %c\n",burn_kerneldtb?'Y':'N');
	install_log("Burning Rescue DTB [Y/N]: %c\n",burn_rescuedtb?'Y':'N');
	install_log("Burning Rescue Rootfs [Y/N]: %c\n",burn_rescue_rootfs?'Y':'N');
	install_log("Burning Bluecore [Y/N]: %c\n",burn_bluecore?'Y':'N');
	install_log("Burning Factory [Y/N]: %c\n",burn_factory?'Y':'N');
	install_log("Burning BootLogo [Y/N]: %c\n",burn_bootlogo?'Y':'N');

	if (burn_factory) {
		install_log("Installing Factory...\n");
		install_factory();
	}

	if (burn_fwtbl)
	{
		if ((update_1stfw == 1) && (update_2ndfw == 1)) {
			uint64_t storage_addr_1stfw_bootlogo = 0;
			uint64_t storage_addr_2ndfw_bootlogo = 0;
			for (i = 0; i < count_1stfw; i++) {
				if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_1stfw_bootlogo = p1stFWDesc[i].offset;
				}
			}
			for (i = 0; i < count_2ndfw; i++) {
				if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_2ndfw_bootlogo = p2ndFWDesc[i].offset;
				}
			}
			if ( (storage_addr_1stfw_bootlogo != 0) && (storage_addr_2ndfw_bootlogo != 0) ) {
				if ( storage_addr_1stfw_bootlogo == storage_addr_2ndfw_bootlogo ) {
					is_bootlogo_burn_once=1;
				}
			}
		}
		if (update_1stfw)
		{
			install_log("Erasing 1st Firmware Table...\n");
			run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%ld bs=%ld count=%ld", align_size, addr_1stfwtbl/align_size, align_size, fwtbl_size/align_size);
			install_log("Writing 1st Firmware Table...\n");
			run("dd if=/tmp/tbl_1stfw.bin of=/dev/mmcblk0 obs=%ld seek=%ld", align_size, addr_1stfwtbl/align_size);
			install_log("Burning 1st Firmware, count %d%s\n", count_1stfw, !count_1stfw? ", exit!":"");
			if (count_1stfw != 0)
			{
				char *file;

				for(i=0; i<count_1stfw; i++)
				{
					if ((file=search_fw_filename(p1stFWDesc[i].type))==NULL)
					{
						install_fail("typecode %d not found\n",p1stFWDesc[i].type);
						exit(1);
					}
					
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;

					install_log("[1st FW]Erasing for %s\n", file);
					run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%lld bs=%ld count=%lu", align_size, p1stFWDesc[i].offset/align_size, align_size, p1stFWDesc[i].paddings/align_size);
					install_log("[1st FW]Writing for %s\n", file);
					run("dd if=/tmp/%s of=/dev/mmcblk0 obs=%ld seek=%lld", file, align_size, p1stFWDesc[i].offset/align_size);
				}
			}
		}
		if (update_2ndfw)
		{
			install_log("Erasing 2nd Firmware Table...\n");
			run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%ld bs=%ld count=%ld", align_size, addr_2ndfwtbl/align_size, align_size, fwtbl_size/align_size);
			install_log("Writing 2nd Firmware Table...\n");
			run("dd if=/tmp/tbl_2ndfw.bin of=/dev/mmcblk0 obs=%ld seek=%ld", align_size, addr_2ndfwtbl/align_size);
			install_log("Burning 2nd Firmware, count %d%s\n", count_2ndfw, !count_2ndfw? ", exit!":"");
			
			if (count_2ndfw != 0)
			{
				char *file;
		
				for(i=0; i<count_2ndfw; i++)
				{
					if ((file=search_fw_filename(p2ndFWDesc[i].type))==NULL)
					{
						install_fail("typecode %d not found\n",p2ndFWDesc[i].type);
						exit(1);
					}
					
					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(is_bootlogo_burn_once)) {
						install_log("[2nd FW] The BootLogo in 1st and 2nd fw table"
									  "are the same, do nothing\n");
						continue;
					}

					install_log("[2nd FW]Erasing for %s\n", file);
					run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%lld bs=%ld count=%lu", align_size, p2ndFWDesc[i].offset/align_size, align_size, p2ndFWDesc[i].paddings/align_size);
					install_log("2nd FW:Writing for %s\n", file);
					run("dd if=/tmp/%s of=/dev/mmcblk0 obs=%ld seek=%lld", file, align_size, p2ndFWDesc[i].offset/align_size);
				}
			}
		}
		install_log("Buring Partition Table\n");
		if (burn_partition_table)
		{
			install_log("Erasing MBR\n");
			run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=0 bs=%ld count=1", align_size, align_size);
			install_log("Writing MBR\n");
			for (i = 0; i < parted_cmd_len; i++)
			{
				run("%s", parted_cmd[i]);
				free(parted_cmd[i]);
			}

			// Reload partition table in order to mkswap on newly-created partition
			install_log("Re-read /dev/mmcblk0 Partition Table\n");
			re_read_partition_table("/dev/mmcblk0");
		}

		install_log("Burning Boot Partition\n");
		if (burn_bootpart)
		{
			install_log("[Partition]Erasing for %s\n", bootpart.file);
			run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%llu bs=%ld count=%lu", align_size, bootpart.address/align_size, align_size, bootpart.zone_size/align_size);
			install_log("[Partition]Writing for %s\n", bootpart.file);
			run("dd if=/tmp/%s of=/dev/mmcblk0 obs=%ld seek=%llu", bootpart.file, align_size, bootpart.address/align_size);
		}

		install_log("Burning Normal Partition\n");
		for (i = 0; i < count_normalpart; i++)
		{
			install_log("[Partition]Erasing for %s\n", normalpart[i].file);
			run("dd if=/dev/zero of=/dev/mmcblk0 obs=%ld seek=%llu bs=%ld count=%lu"
				,align_size
				,normalpart[i].address/align_size
				,align_size
				,normalpart[i].zone_size/align_size);

			if (strcmp(normalpart[i].fs_name, "swap") == 0)
			{
				/* i+2: 1 is bootpart, normalpart starts from 2 */
				install_log("[Partition]Enable swap\n");
				run("mkswap -L swap /dev/mmcblk0p%u", i+2);
			}
			else if (strcmp(normalpart[i].fs_name, "overlay") != 0)
			{
				install_log("[Partition]Writing for %s\n", normalpart[i].file);
				run("dd if=/tmp/%s of=/dev/mmcblk0 obs=%ld seek=%llu"
					,normalpart[i].file
					,align_size
					,normalpart[i].address/align_size);
			}
		}
	}
}

static void run_burn_nand(uint32_t count_1stfw, fw_desc_entry_v2_t *p1stFWDesc, uint32_t count_2ndfw, fw_desc_entry_v2_t *p2ndFWDesc)
{
	uint32_t is_bootlogo_burn_once = 0;
	uint32_t i;
	install_log("Burning NAND\n");
	install_log("Burning Firmware Table [Y/N]: %c\n",burn_fwtbl?'Y':'N');
	install_log("Burning 1st Firmware Table [Y/N]: %c\n",update_1stfw?'Y':'N');
	install_log("Burning 2nd Firmware Table [Y/N]: %c\n",update_2ndfw?'Y':'N');
	install_log("Burning Kernel [Y/N]: %c\n",burn_kernel?'Y':'N');
	install_log("Burning Kernel DTB [Y/N]: %c\n",burn_kerneldtb?'Y':'N');
	install_log("Burning Rescue DTB [Y/N]: %c\n",burn_rescuedtb?'Y':'N');
	install_log("Burning Rescue Rootfs [Y/N]: %c\n",burn_rescue_rootfs?'Y':'N');
	install_log("Burning Bluecore [Y/N]: %c\n",burn_bluecore?'Y':'N');
	install_log("Burning Factory [Y/N]: %c\n",burn_factory?'Y':'N');
	install_log("Burning BootLogo [Y/N]: %c\n",burn_bootlogo?'Y':'N');
	install_log("Burning initramfs [Y/N]: %c\n",burn_initramfs?'Y':'N');

	if (burn_factory) {
		install_log("Installing Factory...\n");
		install_factory();
	}

	if (burn_fwtbl)
	{
		if ((update_1stfw == 1) && (update_2ndfw == 1)) {
			uint32_t i;
			uint64_t storage_addr_1stfw_bootlogo = 0;
			uint64_t storage_addr_2ndfw_bootlogo = 0;
			for (i = 0; i < count_1stfw; i++) {
				if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_1stfw_bootlogo = p1stFWDesc[i].offset;
				}
			}
			for (i = 0; i < count_2ndfw; i++) {
				if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE) && (burn_bootlogo)) {
					storage_addr_2ndfw_bootlogo = p2ndFWDesc[i].offset;
				}
			}
			if ( (storage_addr_1stfw_bootlogo != 0) && (storage_addr_2ndfw_bootlogo != 0) ) {
				if ( storage_addr_1stfw_bootlogo == storage_addr_2ndfw_bootlogo ) {
					is_bootlogo_burn_once=1;
				}
			}
		}
		if (update_1stfw)
		{
			install_log("Erasing 1st Firmware Table...\n");
			run("/tmp/flash_erase /dev/mtd0 0x%lx %ld", addr_1stfwtbl, fwtbl_size/mtd_erasesize);
			install_log("Writing 1st Firmware Table...\n");
			run("/tmp/nandwrite -m -p -s 0x%lx /dev/mtd0 /tmp/tbl_1stfw.bin", addr_1stfwtbl);
			install_log("Burning 1st Firmware, count %d%s\n", count_1stfw, !count_1stfw? ", exit!":"");
			if (count_1stfw != 0)
			{
				uint32_t i;
				char *file;

				for(i=0; i<count_1stfw; i++)
				{
					if ((file=search_fw_filename(p1stFWDesc[i].type))==NULL)
					{
						install_fail("typecode %d not found\n",p1stFWDesc[i].type);
						exit(1);
					}

					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;
					if ((p1stFWDesc[i].type==FW_TYPE_KERNEL_ROOTFS)&&(!burn_initramfs))
						continue;

					install_log("[1st FW] Erasing for %s\n", file);
					run("/tmp/flash_erase /dev/mtd0 0x%llx %u", p1stFWDesc[i].offset, p1stFWDesc[i].paddings/mtd_erasesize);
					install_log("[1st FW] Writing for %s\n", file);
					run("/tmp/nandwrite -m -p -s 0x%llx /dev/mtd0 /tmp/%s", p1stFWDesc[i].offset, file);
				}
			}

		}
		if (update_2ndfw)
		{
			install_log("Erasing 2nd Firmware Table...\n");
			run("/tmp/flash_erase /dev/mtd0 0x%lx %ld", addr_2ndfwtbl, fwtbl_size/mtd_erasesize);
			install_log("Writing 2nd Firmware Table...\n");
			run("/tmp/nandwrite -m -p -s 0x%lx /dev/mtd0 /tmp/tbl_2ndfw.bin", addr_2ndfwtbl);
			install_log("Burning 2nd Firmware, count %d%s\n", count_2ndfw, !count_2ndfw? ", exit!":"");
			if (count_2ndfw != 0)
			{
				uint32_t i;
				char *file;

				for(i=0; i<count_2ndfw; i++)
				{
					if ((file=search_fw_filename(p2ndFWDesc[i].type))==NULL)
					{
						printf("typecode %d not found\n",p2ndFWDesc[i].type);
						exit(1);
					}

					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL)&&(!burn_kernel))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_KERNEL_DT)&&(!burn_kerneldtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_ROOTFS)&&(!burn_rescue_rootfs))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_RESCUE_DT)&&(!burn_rescuedtb))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_AUDIO)&&(!burn_bluecore))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(!burn_bootlogo))
						continue;
					if ((p2ndFWDesc[i].type==FW_TYPE_IMAGE_FILE)&&(is_bootlogo_burn_once)) {
						install_debug("[2nd FW] The BootLogo in 1st and 2nd fw table"
									  "are the same, do nothing\n");
						continue;
					}

					install_log("[2nd FW] Erasing for %s\n", file);
					run("/tmp/flash_erase /dev/mtd0 0x%llx %u", p2ndFWDesc[i].offset, p2ndFWDesc[i].paddings/mtd_erasesize);
					install_log("[2nd FW] Writing for %s\n", file);
					run("/tmp/nandwrite -m -p -s 0x%llx /dev/mtd0 /tmp/%s", p2ndFWDesc[i].offset, file);
				}
			}
		}

		install_log("Burning Boot Partition\n");
		if (bootpart.file[0] != '\0')
		{
			install_log("[Partition]Erasing and writing for %s\n", bootpart.file);
			// usage: ubiformat -f <flash-image> -b <start-offset> -t <total-size>
			run("/tmp/ubiformat /dev/mtd0 -y -c -f /tmp/%s -b 0x%llx -t 0x%x", bootpart.file, bootpart.address, bootpart.zone_size);
		}

		install_log("Burning Normal Partition\n");
		for (i = 0; i < count_normalpart; i++) {
			install_log("[Partition]Erasing and writing for %s\n", normalpart[i].file);
			run("/tmp/ubiformat /dev/mtd0 -y -c -f /tmp/%s -b 0x%llx -t 0x%x", normalpart[i].file, normalpart[i].address, normalpart[i].zone_size);
		}

	}
}

static unsigned int get_meminfo(char *dev)
{
	mtd_info_t mtd_info;
	int dev_fd;

	dev_fd = open(dev, O_RDWR);
	if (dev_fd < 0) {
		install_fail("Fail to open %s: %s\n", dev, strerror(errno));
		exit(1);
	}
	if (ioctl(dev_fd, MEMGETINFO, &mtd_info) < 0) {
		install_fail("ioctl fail: %s\n", strerror(errno));
		exit(1);
	}
	close(dev_fd);

	return mtd_info.erasesize;
}

static long get_nand_factory_start_addr(unsigned int nand_erase_size)
{
	uint64_t erase_size_ull = (uint64_t) nand_erase_size;
	uint64_t factory_zone_ull = (uint64_t) factory_zone;
	uint64_t rsv_size = 0;
	uint64_t ret = 0;

	/* subsitute default erase size (0x20000) with nand_erase_size */
	switch(factory_start_addr)
	{
		/* KYLIN NAND (NAS) */
		case 0x4C0000:
			/* CONFIG_SYS_NO_BL31 */
			rsv_size = erase_size_ull * (6+4+4);
			rsv_size += SIZE_ALIGN_BOUNDARY_MORE(0xC0000, erase_size_ull)*4;
			rsv_size += SIZE_ALIGN_BOUNDARY_MORE(factory_zone_ull, erase_size_ull);
			ret = rsv_size - factory_zone_ull;
			break;

		/* KYLIN NAND */
		case 0x940000:
			rsv_size = erase_size_ull * (6+4+4+4+4+4);
			rsv_size += SIZE_ALIGN_BOUNDARY_MORE(0xC0000, erase_size_ull)*(4+4);
			rsv_size += SIZE_ALIGN_BOUNDARY_MORE(factory_zone_ull, erase_size_ull);
			ret = rsv_size - factory_zone_ull;
			break;

		/* THOR NAND */
		case 0xE60000:
			if (erase_size_ull == 0x40000)
				ret = erase_size_ull * 70;
			else
				ret = erase_size_ull * 115;
			break;

		default:
			break;
	}
	return (long)ret;
}

int main (int argc, char* argv[])
{
	FILE *file = NULL, *target=NULL;
	struct stat stat_buf;
	char newline[512] = {0};
	uint32_t count_1stfw=0, count_2ndfw=0;
	size_t write_total=0;
	char* padding=NULL;
	int fake_run=0;
	fw_desc_table_v1_t *p1stFWTblDesc, *p2ndFWTblDesc;
	fw_desc_entry_v2_t *p1stFWDesc, *p2ndFWDesc;
	part_desc_entry_v1_t *pPartDesc;

	if (argc != 2)
	{
		if (argc != 3)
		{
			install_fail("%s\n", USAGE);
			exit(0);
		}
		else
		{
			fake_run=0x80;
		}
		
	}
	if (stat(argv[1], &stat_buf)!=0)
	{
		install_fail("%s %d %s not found\n",__FILE__,__LINE__,argv[1]);
		exit(0);
	}

	untar_config_txt("tar xvf %s -C /tmp",argv[1]);
	file = fopen("/tmp/config.txt", "r");
	if(file == NULL) {
		install_fail("Can't open %s\r\n", "/tmp/config.txt");
		exit(0);
	}

	pPartDesc=(part_desc_entry_v1_t*)calloc(MAX_PART_LEN, FWTBL_ZONE_SIZE);
	install_log("Allocate partition descriptor %p(%d)\n", pPartDesc, FWTBL_ZONE_SIZE);

	if (pPartDesc == NULL)
	{
		install_fail("Allocate partition descriptor fail \r\n");
		exit(0);
	}
	p1stFWTblDesc=(fw_desc_table_v1_t*)calloc(1, FWTBL_ZONE_SIZE);
	install_log("Allocate firmware table 1 descriptor %p(%d)\n", p1stFWTblDesc, FWTBL_ZONE_SIZE);
	if (p1stFWTblDesc == NULL)
	{
		install_fail("Allocate firmware table 1 descriptor fail \r\n");
		exit(0);
	}
	p2ndFWTblDesc=(fw_desc_table_v1_t*)calloc(1, FWTBL_ZONE_SIZE);
	install_log("Allocate firmware table 2 descriptor %p(%d)\n", p2ndFWTblDesc, FWTBL_ZONE_SIZE);
	if (p2ndFWTblDesc == NULL)
	{
		install_fail("Allocate firmware table 2 descriptor fail \r\n");
		exit(0);
	}
	p1stFWDesc=(fw_desc_entry_v2_t*)calloc(1, FWTBL_ZONE_SIZE);
	install_log("Allocate firmware 1 descriptor %p(%d)\n", p1stFWDesc, FWTBL_ZONE_SIZE);
	if (p1stFWDesc == NULL)
	{
		install_fail("Allocate firmware 1 descriptor fail \r\n");
		exit(0);
	}
	p2ndFWDesc=(fw_desc_entry_v2_t*)calloc(1, FWTBL_ZONE_SIZE);
	install_log("Allocate firmware 2 descriptor %p(%d)\n", p2ndFWDesc, FWTBL_ZONE_SIZE);
	if (p2ndFWDesc == NULL)
	{
		install_fail("Allocate firmware 2 descriptor fail \r\n");
		exit(0);
	}

	while(NULL != fgets(newline, sizeof(newline), file)) {
		if((newline[0] == ';') || (newline[0] == '#')) continue;

		del_cr_lf(newline);
		create_yn_match(use_spi)
		create_yn_match(use_nand)
		create_yn_match(use_emmc)
		create_var_match(align_size)
	}

	if (!use_spi && use_nand && !use_emmc)
	{
		strncpy(storage_dev, "/dev/mtd0", sizeof(storage_dev));
		mtd_erasesize = get_meminfo(storage_dev);
		fwtbl_size = SIZE_ALIGN_BOUNDARY_MORE(0x8000, (uint64_t)mtd_erasesize);
	}
	else
	{
		fwtbl_size = SIZE_ALIGN_BOUNDARY_MORE(0x8000, (uint64_t)align_size);
	}

	fseek(file, 0, SEEK_SET);

	install_log("Reading config.txt\n");
	while(NULL != fgets(newline, sizeof(newline), file)) {
		install_info("%s\n", newline);
		if((newline[0] == ';') || (newline[0] == '#')) continue;

		del_cr_lf(newline);
		create_var_match(debug_level)
	
		create_yn_match(update_1stfw)
		create_yn_match(update_2ndfw)
		create_var_match(seqnum_1stfw)
		create_var_match(seqnum_2ndfw)

		create_yn_match(burn_factory)
		create_var_match(factory_start_addr)
		create_var_match(factory_zone)
		create_yn_match(burn_fwtbl)
		create_yn_match(burn_kernel)
		create_yn_match(burn_kerneldtb)
		create_yn_match(burn_rescuedtb)
		create_yn_match(burn_rescue_rootfs)
		create_yn_match(burn_bluecore)
		create_yn_match(burn_bootlogo)
		create_yn_match(burn_initramfs)
		create_yn_match(burn_bootpart)
		if (update_1stfw==1)
		{
			if (align_size==(-1))
			{
				install_fail("Please define align_size in feeds.conf before\n ... = %s\n", newline);
				exit(1);
			}			
			if ( 0 == strncmp("1stfw", newline, 5) ) {
				add_firmware(skip_char(newline+5, '='), &p1stFWDesc[count_1stfw]);
				count_1stfw++;
				continue;
			}
		}

		if (update_2ndfw==1)
		{
				if (align_size==(-1))
				{
					install_fail("Please define align_size in feeds.conf before\n ... = %s\n", newline);
					exit(1);
				}
				if ( 0 == strncmp("2ndfw", newline, 5) ) {
				add_firmware(skip_char(newline+5, '='), &p2ndFWDesc[count_2ndfw]);
				count_2ndfw++;
				continue;
			}
		}
		if ( 0 == strncmp("bootpart", newline, 8) ) {
			add_partition(skip_char(newline+8, '=')
				, &pPartDesc[count_part++]
				, &bootpart);
			continue;
		}
		if ( 0 == strncmp("normalpart", newline, 10) ) {
			add_partition(skip_char(newline+10, '=')
				,&pPartDesc[count_part++]
				,&normalpart[count_normalpart++]);
			continue;
		}
		if ( 0 == strncmp("factory", newline, 7) ) {
			add_factory(skip_char(newline+7, '='));
			continue;
		}
	}
	install_info("close config.txt\n");
	fclose(file);

	// PartDesc will not be used for NAND since partitions are specified via kernelargs
	if (use_nand)
		count_part = 0;

	install_log("1stfw count %d\n",count_1stfw);
	install_log("2ndfw count %d\n",count_2ndfw);
	install_log("partition count %d\n", count_part);
	
	if ((use_spi + use_nand + use_emmc) !=1 )
	{	
		install_fail("Please define storage type [use_spi|use_nand|use_emmc] in feed.conf\n use_spi=%d\n use_nand=%d\n use_emmc=%d\n", use_spi, use_nand, use_emmc);
		exit(1);
	}

	debug_level|=fake_run;
	if (update_1stfw==1)
	{
		install_log("Building 1st FW table\n");
		fill_fwtbl(p1stFWTblDesc, count_1stfw, p1stFWDesc, seqnum_1stfw, pPartDesc);
		install_log("Generating /tmp/tbl_1stfw.bin\n");
		target=fopen("/tmp/tbl_1stfw.bin","w");
		if(target == NULL) {
			install_fail("Can't open tbl_1stfw.bin\r\n");
			exit(1);
		}
		write_total=fwrite(p1stFWTblDesc, 1, sizeof(fw_desc_table_v1_t), target);
		write_total+=fwrite(pPartDesc, 1, p1stFWTblDesc->part_list_len, target);
		write_total+=fwrite(p1stFWDesc, 1, p1stFWTblDesc->fw_list_len, target);
		write_total=(size_t)fwtbl_size-write_total;
		padding=(char*)calloc(1, write_total);
		fwrite(padding,1, write_total, target);
		fclose(target);
		free(padding);
	}
	
	if (update_2ndfw==1)
	{
		install_log("Building 2nd FW table\n");	
		fill_fwtbl(p2ndFWTblDesc, count_2ndfw, p2ndFWDesc, seqnum_2ndfw, pPartDesc);
		install_log("Generating /tmp/tbl_2ndfw.bin\n");
		target=fopen("/tmp/tbl_2ndfw.bin","w");
		if(target == NULL) {
			install_fail("Can't open tbl_2ndfw.bin\r\n");
			exit(1);
		}
		write_total=fwrite(p2ndFWTblDesc, 1, sizeof(fw_desc_table_v1_t), target);
		write_total+=fwrite(pPartDesc, 1, p2ndFWTblDesc->part_list_len, target);
		write_total+=fwrite(p2ndFWDesc, 1, p2ndFWTblDesc->fw_list_len, target);
		write_total=(size_t)fwtbl_size-write_total;
		padding=(char*)calloc(1, write_total);
		fwrite(padding,1, write_total, target);
		fclose(target);
		free(padding);
	}

	if (use_spi && !use_nand && !use_emmc)	
	{
		addr_1stfwtbl = 0x100000;
		addr_2ndfwtbl = 0x108000;
		strncpy(storage_dev, "/dev/mtd0", sizeof(storage_dev));
		run_burn_spi(count_1stfw, p1stFWDesc, count_2ndfw, p2ndFWDesc);
	}

	if (!use_spi && !use_nand && use_emmc)	
	{
		addr_1stfwtbl = factory_start_addr+factory_zone;
		addr_2ndfwtbl = factory_start_addr+factory_zone+fwtbl_size;
		strncpy(storage_dev, "/dev/mmcblk0", sizeof(storage_dev));
		prepare_emmc_MBR_for_parted();
		run_burn_emmc(count_1stfw, p1stFWDesc, count_2ndfw, p2ndFWDesc, pPartDesc);
	}

	if (!use_spi && use_nand && !use_emmc)
	{
		install_log("NAND erase size: 0x%x\n", mtd_erasesize);
		/* Erasesize is not default value (128KiB). */
		if (mtd_erasesize != 0x20000)
		{
			factory_start_addr = get_nand_factory_start_addr(mtd_erasesize);
			install_log("Factory Start Addr: 0x%lx.\n", factory_start_addr);
		}

		addr_1stfwtbl = factory_start_addr + factory_zone;
		addr_2ndfwtbl = factory_start_addr + factory_zone + fwtbl_size;
		run_burn_nand(count_1stfw, p1stFWDesc, count_2ndfw, p2ndFWDesc);
	}

	free(pPartDesc);
	free(p1stFWTblDesc);
	free(p1stFWDesc);
	free(p2ndFWTblDesc);
	free(p2ndFWDesc);
	install_log("Finish\n");
	return 0;
}
