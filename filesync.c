/*
    Copyright (C) 2012  rofl0r

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/
#undef _GNU_SOURCE
#define _GNU_SOURCE
#include "../lib/include/timelib.h"
#include "../lib/include/filelist.h"
#include "../lib/include/filelib.h"
#include "../lib/include/strlib.h"
#include "../lib/include/logger.h"
#include "../lib/include/crc32c.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <utime.h>
#include <arpa/inet.h>

typedef struct {
	uint64_t symlink;
	uint64_t copies;
	uint64_t skipped;
	uint64_t copied;
} totals;

typedef struct {
	stringptr srcdir_b;
	stringptr* srcdir;
	stringptr dstdir_b;
	stringptr* dstdir;
	
	totals total;
	
	int doChecksum:1;
	int checkChecksum:1;
	int checkFileSize:1;
	int checkDate:1;
	int skipIfNewer:1;
} progstate_s;

static progstate_s progstate;

static int isdir(stringptr* file) {
	return file->size && file->ptr[file->size -1] == '/';
}

static void copyDate(stringptr* file, struct stat* st) {
	struct utimbuf ut;
	ut.modtime = st->st_mtime;
	ut.actime = st->st_atime;
	if(utime(file->ptr, &ut) == -1)
		log_perror("utime");
}

//FIXME for some reason, copyDate seems to have no effect on directories
// maybe another trailing slash issue ?
static void makeDir(stringptr* src, stringptr* dst) {
	struct stat ss;
	if(stat(src->ptr, &ss) == -1) {
		log_perror("stat");
		return;
	}
	if(mkdir(dst->ptr, ss.st_mode) == -1) {
		log_perror("mkdir");
		return;
	}
	copyDate(dst, &ss);
}

static char* getMbsString(char* mbs_buf, size_t buf_size, uint64_t bytes, long ms) {
	float mbs = bytes ? 
		(((float) bytes / (1024.f * 1024.f)) /
		((float) ms / 1000.f)) : 
		0.f;
	unsigned mbs_a = (unsigned) mbs;
	unsigned mbs_b = (unsigned)((mbs - mbs_a) * 100.f);
	ulz_snprintf(mbs_buf, buf_size, "%u.%.2u MB/s", mbs_a, mbs_b);
	return mbs_buf;
}

static void doSync(stringptr* src, stringptr* dst, struct stat *src_stat) {
	int fds, fdd;
	
	int errclose;
	stringptr* err_data;
	char* err_func;
	
	uint64_t done = 0;
	CRC32C_CTX crc;
	union {
		uint32_t asInt;
		uint8_t asChar[4];
	} crc_result;
	
	struct timeval starttime;
	char buf[src_stat->st_blksize];
	
	if((fds = open(src->ptr, O_RDONLY)) == -1) {
		err_data = src;
		err_func = "open";
		errclose = 0;
		
		error:
		
		log_puts(2, err_data);
		log_puts(2, SPL(" "));
		log_perror(err_func);
		if(errclose) {
			close(fds);
			errclose--;
			if(errclose) {
				close(fdd);
				errclose--;
			}
		}
		return;
	};
	if((fdd = open(dst->ptr, O_WRONLY | O_CREAT | O_TRUNC, src_stat->st_mode)) == -1) {
		err_data = dst;
		errclose = 1;
		err_func = "open";
		goto error;
	};

	CRC32C_Init(&crc);
	gettimestamp(&starttime);
	while(done < (uint64_t) src_stat->st_size) {
		ssize_t nread = read(fds, buf, src_stat->st_blksize);
		if(nread == -1) {
			err_data = src;
			err_func = "read";
			errclose = 2;
			goto error;
		} else if (nread == 0) 
			break;
		else {
			ssize_t nwrote = 0, nwrote_s;
			
			CRC32C_Update(&crc, (const uint8_t*) buf, nread);
			
			while(nwrote < nread) {
				nwrote_s = write(fdd, buf + nwrote, nread - nwrote); 
				if(nwrote_s == -1) {
					err_data = dst;
					errclose = 2;
					err_func = "write";
					goto error;
				}
				nwrote += nwrote_s;
			}
			done += nread;
		}
	}
	close(fds);
	close(fdd);
	copyDate(dst, src_stat);
	char crc_str[16];
	char mbs_str[64];
	CRC32C_Final(crc_result.asChar, &crc);
	ulz_snprintf(crc_str, sizeof(crc_str), "%.8x", htonl(crc_result.asInt));
	
	// we do not use printf because it has a limited buffer size
	log_put(1, VARISL("CRC: "), VARIC(crc_str), VARISL(", "), 
		VARIS(src), VARISL(" -> "), VARIS(dst), 
		VARISL(" @"), VARIC(getMbsString(mbs_str, sizeof(mbs_str), src_stat->st_size, mspassed(&starttime))), NULL);
	
	progstate.total.copied += src_stat->st_size;
	progstate.total.copies += 1;
}

static void doFile(stringptr* src, stringptr* dst, struct stat* ss) {
	struct stat sd;
	if(stat(dst->ptr, &sd) == -1) {
		switch(errno) {
			case ENOENT:
				doSync(src, dst, ss);
				return;
			default:
				log_puts(2, dst);
				log_puts(2, SPL(" "));
				log_perror("stat dest");
				break;
		}
	}
	if(ss->st_mtime < sd.st_mtime) {
		ulz_fprintf(2, "dest is newer than source: %s , %s : %llu , %llu\n", src->ptr, dst->ptr, ss->st_mtime, sd.st_mtime);
		if(progstate.skipIfNewer) return;
	}
	if(
		(progstate.checkFileSize && ss->st_size != sd.st_size) ||
		(progstate.checkDate && ss->st_mtime != sd.st_mtime)
	) {
		doSync(src, dst, ss);
		return;
	} else if(progstate.checkChecksum) {
		/* TODO launch 2 processes, each computing the CRC of src/dest in parallel, 
		 * then join em and compare crc and warn and copy if different
		 */
	}
	
	progstate.total.skipped += 1;
}

static void restoreTrailingSlash(stringptr* s) {
	s->ptr[s->size] = '/';
	s->size++;
	s->ptr[s->size] = 0;
}

// FIXME dont copy symlink if the target is equal
// FIXME dont increment progstate.total.symlink in case of failure
static void doLink(stringptr* src, stringptr* dst, struct stat* ss) {
	char buf[4096 + 1];
	int wasdir = 0;
	struct stat sd;
	ssize_t ret = readlink(src->ptr, buf, sizeof(buf) - 1);
	if(ret == -1) {
		log_puts(2, src);
		log_puts(2, SPL(" "));
		log_perror("readlink");
		return;
	} else if (!ret) {
		log_puts(2, src);
		log_puts(2, SPL(" "));
		log_puts(2, SPL("readlink returned 0"));
		return;
	}
	buf[ret] = 0;
	
	if(isdir(dst)) {
		stringptr_shiftleft(dst, 1);
		wasdir = 1;
	}
	
	if(!(lstat(dst->ptr, &sd) == -1 && errno == ENOENT)) {
		//dst already exists, we need to unlink it for symlink to succeed
		//if(S_ISLNK(sd.st_mode))
		if(unlink(dst->ptr) == -1) {
			log_puts(2, dst);
			log_puts(2, SPL(" "));
			log_perror("unlink");
		}
	}
	
	if(symlink(buf, dst->ptr) == -1) {
		log_putc(2, buf);
		log_puts(2, SPL(" -> "));
		log_puts(2, dst);
		log_puts(2, SPL(" "));
		log_perror("symlink");
	} else {
		log_putc(1, buf);
		log_puts(1, SPL(" >> "));
		log_puts(1, dst);
		log_putln(1);
	}
	
	//updating the link's timestamp does in fact update the timestamp
	//of the file it points to, which can confuse other parts of the program.
	//so we dont do it.
	//copyDate(dst, ss);
	(void) ss;
	
	if(wasdir) {
		restoreTrailingSlash(dst);
	}
	
	progstate.total.symlink += 1;
}

static void doDir(stringptr* subd) {
	filelist f;
	stringptr *combined_src = stringptr_concat(progstate.srcdir, subd, NULL);
	stringptr *combined_dst = stringptr_concat(progstate.dstdir, subd, NULL);
	struct stat src_stat;
	
	if(!filelist_search(&f, combined_src, SPL("*"), FLF_EXCLUDE_PATH | FLF_INCLUDE_HIDDEN)) {
		stringptr* file;
		stringptr* file_combined_src;
		stringptr* file_combined_dst;
		sblist_iter(f.files, file) {
			file_combined_src = stringptr_concat(combined_src, file, NULL);
			file_combined_dst = stringptr_concat(combined_dst, file, NULL);
			if(isdir(file)) {
				// remove trailing slash so stat doesnt resolve symlinks...
				stringptr_shiftleft(file_combined_src, 1);
			}
			if(lstat(file_combined_src->ptr, &src_stat) == -1) {
				log_puts(2, file_combined_src);
				log_puts(2, SPL(" "));
				log_perror("stat");
			} else {
				if(S_ISLNK(src_stat.st_mode)) {
					
					doLink(file_combined_src, file_combined_dst, &src_stat);
					
				} else if(isdir(file)) {
					restoreTrailingSlash(file_combined_src);

					stringptr *path_combined = stringptr_concat(subd, file, NULL);
					if(access(file_combined_dst->ptr, R_OK) == -1 && errno == ENOENT)
						makeDir(file_combined_src, file_combined_dst);
					doDir(path_combined);
					stringptr_free(path_combined);
				} else {
					doFile(file_combined_src, file_combined_dst, &src_stat);
				}
			}	
			stringptr_free(file_combined_src);
			stringptr_free(file_combined_dst);
		}
		filelist_free(&f);
	}
	
	stringptr_free(combined_src);
	stringptr_free(combined_dst);
	
}

static void printStats(long ms) {
	char mbs_buf[64];
	ulz_fprintf(1,  "copied: %llu\n"
			"skipped: %llu\n"
			"symlinks: %llu\n"
			"bytes copied: %llu\n"
			"seconds: %lu\n"
			"rate: %s\n",
			progstate.total.copies, 
			progstate.total.skipped, 
			progstate.total.symlink, 
			progstate.total.copied,
			ms / 1000,
			getMbsString(mbs_buf, sizeof(mbs_buf), progstate.total.copied, ms)
	);
}

static int syntax() {
	log_puts(1, SPL("prog srcdir dstdir\n"));
	return 1;
}

int main (int argc, char** argv) {
	if(argc < 2) return syntax();
	int startarg = 1;
	int freedst = 0;
	struct timeval starttime;
	
	progstate.checkFileSize = 1;
	progstate.checkDate = 1;
	progstate.checkChecksum = 1;
	
	memset(&progstate.total, 0, sizeof(totals));
	
	progstate.srcdir = stringptr_fromchar(argv[startarg], &progstate.srcdir_b);
	progstate.dstdir = stringptr_fromchar(argv[startarg+1], &progstate.dstdir_b);
	
	if(access(progstate.dstdir->ptr, R_OK) == -1 && errno == ENOENT)
						makeDir(progstate.srcdir, progstate.dstdir);
	if(!isdir(progstate.dstdir)) {
		progstate.dstdir = stringptr_concat(progstate.dstdir, SPL("/"), NULL);
		freedst = 1;
	}
	
	gettimestamp(&starttime);
	
	CRC32C_InitTables();
	
	doDir(isdir(progstate.srcdir) ? SPL("") : SPL("/"));
	
	printStats(mspassed(&starttime));
	
	if(freedst) stringptr_free(progstate.dstdir);
	
	return 0;
}
