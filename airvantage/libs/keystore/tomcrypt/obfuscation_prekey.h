/* AES key used to obfuscate actual keys on the filesystem. Change this
 * for any other binary sequence of the same length to improve safety. */
#define OBFUSCATION_PREKEY {                        \
	0x12, 0x95, 0xF2, 0xDC, 0x21, 0x59, 0x2F, 0xCD, \
    0x95, 0x21, 0xDC, 0x2F, 0xFE, 0xDA, 0xBE, 0xBE  }