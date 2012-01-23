#include "../lib/include/crc32c.h"
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

int syntax() {
	puts("prog filename");
	return 1;
}

uint32_t getcrc(char* filename, uint64_t filesize, size_t blocksize) {
	int fd = open(filename, O_RDONLY);
	uint64_t done = 0;
	char* err = NULL;
	int err_close = 0;
	CRC32C_CTX crc;
	union {
		uint32_t asInt;
		uint8_t asChar[4];
	} crc_result;
	char buf[blocksize];
	
	if(fd == -1) {
		err = "open";
		error:
		perror(err);
		if(err_close) close(fd);
		return 0;
	}
	
	CRC32C_Init(&crc);
	while(done < filesize) {
		ssize_t nread = read(fd, buf, blocksize);
		if(nread == -1) {
			err = "read";
			err_close = 1;
			goto error;
		} else if (nread == 0) 
			break;
		else {
			
			CRC32C_Update(&crc, (const uint8_t*) buf, nread);
			done += nread;
		}
	}
	close(fd);
	CRC32C_Final(crc_result.asChar, &crc);
	return crc_result.asInt;
}

int main(int argc, char**argv) {
	if(argc != 2) return syntax();
	struct stat st;
	if(stat(argv[1], &st) == -1) {
		perror("stat");
		return 1;
	}
	CRC32C_InitTables();
	printf("%.8x\n", getcrc(argv[1], st.st_size, st.st_blksize));
	return 0;
}
