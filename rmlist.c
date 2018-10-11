#include <stdio.h>
#include <string.h>
#include <errno.h>

static int usage(const char *argv0) {
	dprintf(2,
		"usage: %s file\n\n"
		"all filenames inside file will be deleted\n"
		"filename may be '-' to indicate stdin\n", argv0);
	return 1;
}

int main(int argc, char** argv) {
	if(argc != 2) return usage(argv[0]);
	char buf[2048];
	FILE *f;
	if(!strcmp(argv[1], "-")) f = stdin;
	else f = fopen(argv[1], "r");
	if(!f) {
		perror("fopen");
		return 1;
	}
	while(fgets(buf, sizeof buf, f)) {
		char *p = strrchr(buf, '\n');
		if(!p) {
			dprintf(2, "error: line too long: %s\n", buf);
			continue;
		}
		*p = 0;
		if(remove(buf) != 0) {
			dprintf(2, "error: %s (%s)\n", strerror(errno), buf);
		} else {
			printf("removed %s\n", buf);
		}
	}
	if(f != stdin) fclose(f);
	return 0;
}
