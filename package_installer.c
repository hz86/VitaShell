#include "package_installer.h"
#include "main.h"

#include "message_dialog.h"
#include "promoterutil.h"
#include "sha1.h"
#include "sysmodule_internal.h"

// for .zip extraction
#include "fex/fex.h"

void closeWaitDialog();

#define DISPLAY_ERROR(fmt, ...) do { \
	closeWaitDialog(); \
	initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK, fmt, ##__VA_ARGS__); \
	dialog_step = DIALOG_STEP_ERROR; \
} while (0);

static int fex_error(fex_err_t err) {
	if (err != NULL) {
		DISPLAY_ERROR("%s", fex_err_str(err));
		return 1;
	}
	return 0;
}

#define PROGRESS(x) sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, x);

#define PACKAGE_PARENT "ux0:ptmp"
#define PACKAGE_DIR PACKAGE_PARENT "/pkg"

static void load_paf() {
	unsigned ptr[0x100] = {0};
	ptr[0] = 0;
	ptr[1] = (unsigned)&ptr[0];
	unsigned scepaf_argp[] = {0x400000, 0xEA60, 0x40000, 0, 0};
	sceSysmoduleLoadModuleInternalWithArg(0x80000008, sizeof(scepaf_argp), scepaf_argp, ptr);
}

int promote(const char *path)
{
	int res;
	int ret;
	int state;

	load_paf();

	ret = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_PROMOTER_UTIL);
	if (ret < 0) {
		DISPLAY_ERROR("sceSysmoduleLoadModuleInternal failed: 0x%x", ret);
		return ret;
	}

	PROGRESS(45);

	ret = scePromoterUtilityInit();
	if (ret < 0) {
		DISPLAY_ERROR("scePromoterUtilityInit failed: 0x%x", ret);
		return ret;
	}

	PROGRESS(50);

	// ret = scePromoterUtilityPromotePkgWithRif(path, 0);
	ret = scePromoterUtilityPromotePkg(path, 0);
	if (ret < 0) {
		DISPLAY_ERROR("scePromoterUtilityPromotePkg failed: 0x%x", ret);
		return ret;
	}

	PROGRESS(55);

	int cnt = 0;

	state = 1;
	do {
		ret = scePromoterUtilityGetState(&state);
		if (ret < 0) {
			DISPLAY_ERROR("scePromoterUtilityGetState failed: 0x%x", ret);
			return ret;
		}
		cnt += 1;
		PROGRESS(MIN(95, 55 + 1 * cnt));
		sceKernelDelayThread(300000);
	} while (state);

	ret = scePromoterUtilityGetResult(&res);
	if (ret < 0) {
		DISPLAY_ERROR("scePromoterUtilityGetResult failed: 0x%x", ret);
		return ret;
	}

	PROGRESS(95);

	ret = scePromoterUtilityExit();
	if (ret < 0) {
		DISPLAY_ERROR("scePromoterUtilityExit failed: 0x%x", ret);
		return ret;
	}

	ret = sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_PROMOTER_UTIL);
	if (ret < 0) {
		DISPLAY_ERROR("sceSysmoduleUnloadModuleInternal failed: 0x%x", ret);
		return ret;
	}

	PROGRESS(100);

	if (res < 0)
		DISPLAY_ERROR("Failed to install the package: 0x%x", res);

	return res;
}

// taken from musl
static char *dirname(char *s)
{
	size_t i;
	if (!s || !*s) return ".";
	i = strlen(s)-1;
	for (; s[i]=='/'; i--) if (!i) return "/";
	for (; s[i]!='/'; i--) if (!i) return ".";
	for (; s[i]=='/'; i--) if (!i) return "/";
	s[i+1] = 0;
	return s;
}

static void mkdirs(const char *dir) {
	char dir_copy[0x400] = {0};
	strncpy(dir_copy, dir, sizeof(dir_copy) - 2);
	dir_copy[strlen(dir_copy)] = '/';
	char *c;
	for (c = dir_copy; *c; ++c) {
		if (*c == '/') {
			*c = '\0';
			sceIoMkdir(dir_copy, 0777);
			*c = '/';
		}
	}
}

static int unzip_file(const char *src, const char *dst) {
	int ret = 0;
	fex_t *fex = NULL;
	// Open the vpk
	if (fex_error(fex_open_type(&fex, src, fex_identify_extension(".zip"))))
		return -1;
	char path[0x400] = {0};
	char dir[0x400] = {0};

	PROGRESS(10);

	// Unpack all files to temporary directory
	while (!fex_done(fex)) {
		const char *name = fex_name(fex);
		snprintf(path, sizeof(path), "%s/%s", dst, name);
		strcpy(dir, path);
		mkdirs(dirname(dir));

		int fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 7);
		if (fd < 0) {
			DISPLAY_ERROR("Failed opening %s for write: 0x%x", path, fd);
			return -1;
		}
		const void *data;
		int size;
		if (fex_error(fex_data(fex, &data)))
			return -1;
		size = fex_size(fex);
		ret = sceIoWrite(fd, data, size);
		if (ret != size) {
			DISPLAY_ERROR("Failed writing %s: 0x%x", path, ret);
			return -1;
		}
		sceIoClose(fd);

		if (fex_error(fex_next(fex)))
			return -1;
	}
	
	// Close archive and null pointer to avoid accidental use
	fex_close(fex);
	fex = NULL;

	return 0;
}


typedef struct SfoHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t keyofs;
	uint32_t valofs;
	uint32_t count;
} __attribute__((packed)) SfoHeader;

typedef struct SfoEntry {
	uint16_t nameofs;
	uint8_t  alignment;
	uint8_t  type;
	uint32_t valsize;
	uint32_t totalsize;
	uint32_t dataofs;
} __attribute__((packed)) SfoEntry;

char *get_title_id(const char *filename) {
	char *res = NULL;
	long size = 0;
	FILE *fin = NULL;
	char *buf = NULL;
	int i;

	SfoHeader *header;
	SfoEntry *entry;
	
	fin = fopen(filename, "rb");
	if (!fin)
		goto cleanup;
	if (fseek(fin, 0, SEEK_END) != 0)
		goto cleanup;
	if ((size = ftell(fin)) == -1)
		goto cleanup;
	if (fseek(fin, 0, SEEK_SET) != 0)
		goto cleanup;
	buf = calloc(1, size + 1);
	if (!buf)
		goto cleanup;
	if (fread(buf, size, 1, fin) != 1)
		goto cleanup;

	header = (SfoHeader*)buf;
	entry = (SfoEntry*)(buf + sizeof(SfoHeader));
	for (i = 0; i < header->count; ++i, ++entry) {
		const char *name = buf + header->keyofs + entry->nameofs;
		const char *value = buf + header->valofs + entry->dataofs;
		if (name >= buf + size || value >= buf + size)
			break;
		if (strcmp(name, "TITLE_ID") == 0)
			res = strdup(value);
	}

cleanup:
	if (buf)
		free(buf);
	if (fin)
		fclose(fin);

	return res;
}

unsigned char base_head_bin[] = {
	0x7f, 0x50, 0x4b, 0x47, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x80,
	0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x01, 0x90, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x0a, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x10,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xa6, 0x89, 0x94, 0x38, 0x19, 0xf2, 0xdd, 0x05, 0x87, 0x94, 0xb0, 0xb6,
	0x7f, 0xc9, 0x30, 0x76, 0xdc, 0x2f, 0x22, 0xf2, 0x25, 0x40, 0xc6, 0xdf,
	0x94, 0xcb, 0xb7, 0x78, 0xf8, 0xa2, 0x54, 0x95, 0x8c, 0xe6, 0xfd, 0x74,
	0x81, 0x0c, 0xf7, 0x9d, 0x47, 0xb2, 0x86, 0x60, 0x3c, 0x2e, 0x00, 0xbb,
	0xa2, 0x07, 0x59, 0x51, 0xe7, 0x95, 0xa4, 0xed, 0x83, 0x50, 0x35, 0xbc,
	0x65, 0x63, 0xfe, 0x70, 0x8b, 0xab, 0x0c, 0x49, 0x73, 0x9d, 0xa3, 0xc9,
	0x1f, 0x74, 0x48, 0x22, 0x70, 0x93, 0xfc, 0xe9, 0x40, 0xca, 0x74, 0x97,
	0xba, 0xf1, 0xde, 0x1c, 0xaa, 0x67, 0xb7, 0x41, 0x78, 0xd7, 0x15, 0x68,
	0x7f, 0x65, 0x78, 0x74, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x40,
	0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x10,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0xe0, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x00, 0x00, 0x02, 0x00, 0x00, 0x04, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0a, 0xa2, 0xcb, 0x21,
	0x8b, 0x37, 0x06, 0x2d, 0x3e, 0x05, 0xfa, 0x11, 0x72, 0x72, 0x88, 0x85,
	0xc9, 0x7b, 0x03, 0x99, 0xa0, 0x70, 0x9c, 0xf8, 0xcf, 0x9d, 0x41, 0x01,
	0xd6, 0x17, 0x9f, 0xd3, 0x57, 0x79, 0x67, 0xf9, 0xb6, 0xf8, 0x56, 0x3d,
	0xca, 0xfc, 0xa1, 0x98, 0xe2, 0xc7, 0xcf, 0xd6, 0x2e, 0x1b, 0xd6, 0x1b,
	0xbe, 0x6f, 0xc1, 0x92, 0xbe, 0xe0, 0xb3, 0xc2, 0xe5, 0x65, 0x5a, 0x45,
	0xd9, 0x88, 0xb4, 0x97, 0x5e, 0x16, 0x31, 0x3d, 0xa2, 0x3e, 0x16, 0xae,
	0xd4, 0xb7, 0xd5, 0x36, 0xe3, 0xac, 0x80, 0x8f, 0x18, 0xfe, 0xad, 0x1a,
	0x85, 0x20, 0xce, 0xee, 0xda, 0x5d, 0xb7, 0x95, 0x46, 0x34, 0xcc, 0x49,
	0x52, 0x09, 0xf6, 0xeb, 0xa5, 0x0a, 0xe5, 0x7c, 0xb5, 0x7f, 0xaf, 0x6f,
	0x4c, 0x06, 0x8c, 0xe4, 0xd8, 0x5a, 0x03, 0xaf, 0x92, 0x4e, 0x95, 0x5b,
	0xbc, 0xe0, 0xc2, 0xac, 0xff, 0x12, 0x95, 0x31, 0x92, 0xad, 0x06, 0xe8,
	0x17, 0x2c, 0xb1, 0xdc, 0x36, 0xa4, 0xc3, 0x9b, 0xe2, 0x3e, 0x2b, 0xec,
	0x65, 0x53, 0xeb, 0x58, 0x84, 0x49, 0x09, 0x0b, 0xf4, 0xc6, 0xb4, 0x02,
	0x70, 0xf3, 0x64, 0x58, 0x75, 0x14, 0x00, 0xf8, 0x68, 0x88, 0x46, 0x7e,
	0x5c, 0xbc, 0xbe, 0x8b, 0x5f, 0xac, 0xe0, 0xe4, 0xa6, 0xf5, 0x77, 0xdd,
	0xd9, 0xe5, 0xaf, 0x05, 0xf0, 0x5d, 0xae, 0x22, 0x7f, 0xb4, 0xd1, 0x1c,
	0x7f, 0xcc, 0x3e, 0x98, 0x55, 0xb9, 0x69, 0xd2, 0xd2, 0x10, 0x55, 0x45,
	0x4b, 0x3c, 0x95, 0x70, 0xb7, 0xc3, 0xdb, 0xfe, 0x23, 0xaf, 0xcd, 0x27,
	0xa2, 0xd3, 0xac, 0x8c, 0x11, 0x09, 0xbf, 0xf6, 0xb2, 0x01, 0x62, 0x09,
	0xc1, 0xda, 0xfd, 0xa7, 0x47, 0xa9, 0x48, 0xf4, 0x46, 0x26, 0x06, 0xf2,
	0x76, 0x4d, 0xfe, 0x6f, 0x3f, 0x10, 0xb0, 0x1c, 0x1a, 0xde, 0x73, 0x8b,
	0x14, 0x73, 0x3c, 0x39, 0xb6, 0xc6, 0x1b, 0xa1, 0x65, 0x99, 0xb8, 0x33,
	0xac, 0xb8, 0x16, 0xb4, 0xe6, 0xa5, 0xec, 0x02, 0x0b, 0x5b, 0x70, 0x23,
	0xeb, 0x24, 0x1a, 0xf7, 0x8c, 0xda, 0x55, 0x96, 0xdd, 0x4b, 0x1c, 0x85,
	0x83, 0x49, 0x01, 0xb2, 0x39, 0xbc, 0x31, 0x3b, 0xe8, 0xf1, 0x5a, 0x49,
	0xcc, 0xcf, 0x0f, 0x85, 0x5f, 0x54, 0x79, 0xe8, 0x31, 0x8d, 0x57, 0x1b,
	0xb1, 0xc2, 0x93, 0x87, 0xe2, 0xe6, 0x56, 0xcf, 0x92, 0x51, 0xfc, 0x49,
	0x94, 0xcd, 0xb5, 0x04, 0x1b, 0x04, 0x47, 0xf7, 0xb4, 0xd2, 0x67, 0x31,
	0x54, 0xf0, 0xad, 0x3a, 0xd4, 0x25, 0x8c, 0xed, 0xe9, 0x9b, 0x12, 0xfc,
	0x47, 0x1c, 0xfc, 0x6e, 0x81, 0x29, 0x8b, 0x39, 0xab, 0xbb, 0xf0, 0x35,
	0x00, 0x87, 0x88, 0x87, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x90, 0x00, 0x00, 0x00, 0x00, 0x05,
	0x00, 0x00, 0x00, 0x04, 0x19, 0x67, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08,
	0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x28,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x1a, 0x92, 0x07, 0x04,
	0x61, 0x0c, 0x9d, 0x14, 0x55, 0x8e, 0x17, 0x74, 0xb6, 0x44, 0xd2, 0x5c,
	0x93, 0xf3, 0xc1, 0x58, 0x0f, 0x91, 0x22, 0x2f, 0xfd, 0xb4, 0x42, 0xaa,
	0x64, 0xfc, 0x8a, 0xd0, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x48,
	0x00, 0x00, 0x05, 0x90, 0x00, 0x00, 0x03, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xbe, 0x9a, 0x07, 0x26, 0x1a, 0x91, 0xd6, 0x35,
	0x93, 0xcd, 0x59, 0xf4, 0x13, 0x23, 0x34, 0x05, 0x5b, 0xc4, 0xf5, 0xc3,
	0x31, 0xf3, 0xf9, 0xf1, 0x7e, 0xdb, 0x7f, 0x53, 0x0f, 0x1a, 0x0a, 0x79,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x0a, 0x30,
	0x00, 0x00, 0x00, 0x60, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0x2a, 0xa0, 0xf7,
	0x3a, 0x41, 0x84, 0x7c, 0xb8, 0x66, 0x43, 0x7b, 0xca, 0xcd, 0x68, 0x5e,
	0x44, 0xab, 0xd9, 0x85, 0xc9, 0x6b, 0xad, 0x33, 0xa9, 0xbc, 0x88, 0xc6,
	0x75, 0xc5, 0x23, 0x9e, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x28,
	0x01, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x7a, 0xfe, 0xf5, 0x79, 0x30, 0xaf, 0x76, 0xe0,
	0x46, 0xfc, 0x75, 0xdf, 0x08, 0x4e, 0xb8, 0x45, 0x3d, 0x4f, 0xcb, 0xf4,
	0x3d, 0x9b, 0xfa, 0x5f, 0x61, 0x99, 0x6a, 0xde, 0x9c, 0x2e, 0x1a, 0x9c,
	0x19, 0x15, 0x10, 0x1d, 0x71, 0xe6, 0xc0, 0x5a, 0x84, 0x3d, 0x20, 0xe8,
	0xae, 0x1e, 0x1c, 0x71, 0x94, 0xee, 0xbc, 0x73, 0x4d, 0x2c, 0x46, 0xbf,
	0x3c, 0xf3, 0x5b, 0x30, 0x3a, 0xc3, 0x18, 0x20, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff
};

void fpkg_hmac(const unsigned char *data, unsigned int len, unsigned char hmac[16]) {
	SHA1_CTX ctx;
	unsigned char sha1[20];
	unsigned char buf[64];

	sha1_init(&ctx);
	sha1_update(&ctx, data, len);
	sha1_final(&ctx, sha1);

	memset(buf, 0, 64);
	memcpy(&buf[0], &sha1[4], 8);
	memcpy(&buf[8], &sha1[4], 8);
	memcpy(&buf[16], &sha1[12], 4);
	buf[20] = sha1[16];
	buf[21] = sha1[1];
	buf[22] = sha1[2];
	buf[23] = sha1[3];
	memcpy(&buf[24], &buf[16], 8);

	sha1_init(&ctx);
	sha1_update(&ctx, buf, 64);
	sha1_final(&ctx, sha1);
	memcpy(hmac, sha1, 16);
}

#define ntohl __builtin_bswap32

#define HEAD_BIN PACKAGE_DIR "/sce_sys/package/head.bin"

int make_headbin(void) {
	int ret = 0;
	char *title_id = NULL;
	int fd = 0;
	unsigned char *head_bin = NULL;
	unsigned char hmac[16];
	uint32_t off;
	uint32_t len;
	uint32_t out;
	int hb = 0;
	char full_title_id[128] = {0};

	// head.bin must not be present
	fd = sceIoOpen(HEAD_BIN, SCE_O_RDONLY, 0);
	if (fd > 0) {
		DISPLAY_ERROR("Please remove sce_sys/package/head.bin from your vpk");
		ret = -1;
		goto cleanup;
	}

	title_id = get_title_id(PACKAGE_DIR "/sce_sys/param.sfo");
	if (!title_id) {
		DISPLAY_ERROR("Failed to obtain title id, check that param.sfo is correct");
		ret = -1;
		goto cleanup;
	}

	head_bin = malloc(sizeof(base_head_bin));
	if (!head_bin) {
		DISPLAY_ERROR("Failed to allocate head_bin");
		ret = -1;
		goto cleanup;
	}
	memcpy(head_bin, base_head_bin, sizeof(base_head_bin));

	snprintf(full_title_id, sizeof(full_title_id), "EP9000-%s_00-XXXXXXXXXXXXXXXX", title_id);
	// write titleid
	strncpy((char *)&head_bin[0x30], full_title_id, 48);

	// hmac of pkg header
	len = ntohl(*(uint32_t *)&head_bin[0xD0]);
	fpkg_hmac(&head_bin[0], len, hmac);
	memcpy(&head_bin[len], hmac, 16);

	// hmac of pkg info
	off = ntohl(*(uint32_t *)&head_bin[0x8]);
	len = ntohl(*(uint32_t *)&head_bin[0x10]);
	out = ntohl(*(uint32_t *)&head_bin[0xD4]);
	fpkg_hmac(&head_bin[off], len - 64, hmac);
	memcpy(&head_bin[out], hmac, 16);

	// hmac of everything
	len = ntohl(*(uint32_t *)&head_bin[0xE8]);
	fpkg_hmac(&head_bin[0], len, hmac);
	memcpy(&head_bin[len], hmac, 16);

	mkdirs(PACKAGE_DIR "/sce_sys/package");

	hb = sceIoOpen(HEAD_BIN, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 7);
	if (hb < 0) {
		DISPLAY_ERROR("Failed to open head.bin for write: 0x%x", hb);
		ret = -1;
		goto cleanup;
	}
	if ((ret = sceIoWrite(hb, head_bin, sizeof(base_head_bin))) != sizeof(base_head_bin)) {
		DISPLAY_ERROR("Failed to write head.bin: 0x%x", ret);
		ret = -1;
		goto cleanup;
	}

cleanup:
	if (fd > 0)
		sceIoClose(fd);
	if (title_id)
		free(title_id);
	if (head_bin)
		free(head_bin);
	if (hb > 0)
		sceIoClose(hb);

	return ret;
}

int install_thread(SceSize args_size, InstallArguments *args) {
	int ret;

	PROGRESS(0);

	// recursively clean up package_temp directory
	removePath(PACKAGE_PARENT, NULL, 0, NULL);
	
	// unpack the vpk
	ret = unzip_file(args->file, PACKAGE_DIR);
	if (ret < 0)
		goto end;

	PROGRESS(30);

	// generate head.bin, which is the only file required for installation that's not already present in vpk
	ret = make_headbin();
	if (ret < 0)
		goto end;

	PROGRESS(40);

	ret = promote(PACKAGE_DIR);

end:
	if (dialog_step != DIALOG_STEP_ERROR) {
		PROGRESS(100);
		sceKernelDelayThread(1 * 1000 * 1000);
		sceMsgDialogClose();

		dialog_step = DIALOG_STEP_INSTALLED;
	}

	return sceKernelExitDeleteThread(0);
}
