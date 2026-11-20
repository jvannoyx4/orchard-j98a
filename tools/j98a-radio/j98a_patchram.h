// SPDX-License-Identifier: MIT
/* Included only by the separately built exact-file Patchram diagnostic. */
#define PATCH_BYTES 59360
#define PATCH_RECORDS 295
#define PATCH_SHA "958ee3706c9479017704aab1ea246ff77865628e75eae2d62bcb934cd10ab8a4"
static uint8_t patch[PATCH_BYTES];

/* SHA-256: FIPS 180-4, for checking the complete file before any device open. */
static uint32_t ror32(uint32_t x, unsigned int n) { return (x >> n) | (x << (32 - n)); }
static void sha256(const uint8_t *bytes, size_t length, char hex[65])
{
	static const uint32_t k[64] = {
		0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
		0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
		0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
		0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
		0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
		0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
		0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
		0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
	};
	uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
	size_t padded = (length + 9 + 63) / 64 * 64, base;
	for (base = 0; base < padded; base += 64) {
		uint32_t w[64], a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], z=h[7];
		uint8_t block[64];
		unsigned int i;
		for (i=0; i<64; i++) {
			size_t p = base+i;
			block[i] = p < length ? bytes[p] : p == length ? 0x80 : 0;
			if (p >= padded-8) block[i] = (uint64_t)length * 8 >> ((padded-1-p)*8);
		}
		for (i=0; i<16; i++) w[i] = (uint32_t)block[i*4]<<24 | (uint32_t)block[i*4+1]<<16 | (uint32_t)block[i*4+2]<<8 | block[i*4+3];
		for (i=16; i<64; i++) w[i] = w[i-16] + (ror32(w[i-15],7)^ror32(w[i-15],18)^(w[i-15]>>3)) + w[i-7] + (ror32(w[i-2],17)^ror32(w[i-2],19)^(w[i-2]>>10));
		for (i=0; i<64; i++) {
			uint32_t t1 = z+(ror32(e,6)^ror32(e,11)^ror32(e,25))+((e&f)^(~e&g))+k[i]+w[i];
			uint32_t t2 = (ror32(a,2)^ror32(a,13)^ror32(a,22))+((a&b)^(a&c)^(b&c));
			z=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
		}
		h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=z;
	}
	for (unsigned int i=0; i<8; i++) snprintf(hex+i*8,9,"%08x",h[i]);
}

static int load_patchram(const char *path)
{
	char digest[65];
	unsigned int records = 0;
	size_t offset = 0, n;
	FILE *stream = fopen(path, "rb");
	if (!stream) { perror("open exact USI firmware"); return 1; }
	n = fread(patch, 1, sizeof(patch), stream);
	if (n != sizeof(patch) || fgetc(stream) != EOF || ferror(stream)) {
		fprintf(stderr, "Firmware size mismatch; no device opened.\n"); fclose(stream); return 1;
	}
	fclose(stream);
	sha256(patch, sizeof(patch), digest);
	if (strcmp(digest, PATCH_SHA)) {
		fprintf(stderr, "Firmware SHA256 mismatch; no device opened.\n"); return 1;
	}
	while (offset < sizeof(patch)) {
		unsigned int opcode, length;
		if (sizeof(patch)-offset < 3) return 1;
		opcode = le16(patch+offset); length = patch[offset+2]; offset += 3;
		if (length < 4 || sizeof(patch)-offset < length) return 1;
		if (offset+length == sizeof(patch)) {
			if (opcode != 0xfc4e || length != 4 || memcmp(patch+offset,"\xff\xff\xff\xff",4)) return 1;
		} else if (opcode != 0xfc4c) return 1;
		offset += length; records++;
	}
	if (records != PATCH_RECORDS) return 1;
	printf("{\"firmware\":\"BCM.apple,j98a-USI.hcd\",\"sha256\":\"%s\",\"records\":%u}\n", digest, records);
	return 0;
}

static int run_patchram(int fd)
{
	struct timespec minidrv_settle = { .tv_nsec=50000000 }, launch_settle = { .tv_nsec=250000000 };
	uint8_t reply[9];
	size_t offset = 0;
	unsigned int records = 0;
	/* Do not repeat a download over an already patched or different controller. */
	if (command(fd, 0x1001, 9, reply) || le16(reply+5) != 15 ||
	    le16(reply+7) != 0x6607 || le16(reply+2) != 0x1000) {
		fprintf(stderr, "Expected measured ROM identity/revision before Patchram; stopped.\n"); return 1;
	}
	total_deadline = now_ms() + 45000;
	/* Match btbcm_patchram(): Download Minidrv, 50ms, records, 250ms. */
	if (command(fd, 0xfc2e, 1, reply)) { perror("Download Minidrv"); return 1; }
	if (nanosleep(&minidrv_settle,NULL) || stopping) return 1;
	while (offset < sizeof(patch)) {
		unsigned int opcode = le16(patch+offset), length = patch[offset+2];
		offset += 3;
		if (command_data(fd, opcode, patch+offset, length, 1, reply)) {
			fprintf(stderr, "Patchram stopped at record %u opcode 0x%04x: %s\n", records, opcode, strerror(errno));
			return 1;
		}
		offset += length; records++;
	}
	if (nanosleep(&launch_settle,NULL) || stopping) return 1;
	printf("{\"patchram_records_acknowledged\":%u}\n", records);
	/* Reuse exact Reset/Version probe; never change baud or device address. */
	return run_commands(fd);
}
