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
#include "../lib/include/optparser.h"
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
	stringptr diffdir_b;
	stringptr* diffdir;
	
	totals total;
	
	int doChecksum:1;
	int checkExists:1;
	int checkChecksum:1;
	int checkFileSize:1;
	int checkDate:1;
	int checkDateOlder:1;
	int simulate:1;
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

static void restoreTrailingSlash(stringptr* s) {
	s->ptr[s->size] = '/';
	s->size++;
	s->ptr[s->size] = 0;
}

static inline int removeTrailingSlash(stringptr* s) {
	if(isdir(s)) {
		stringptr_shiftleft(s, 1);
		return 1;
	}
	return 0;
}

static void updateTimestamp(stringptr* dst, struct stat* ss) {
	int wasdir = removeTrailingSlash(dst);
	copyDate(dst, ss);
	if(wasdir) restoreTrailingSlash(dst);
}

static void makeDir(stringptr* dst, struct stat* ss) {
	if(progstate.simulate) 
		return;
	if(mkdir(dst->ptr, ss->st_mode) == -1) {
		log_perror("mkdir");
		return;
	}
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

static void doSync(stringptr* src, stringptr* dst, struct stat *src_stat, char* reason) {
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
	long time_passed;
	
	if(progstate.simulate) {
		crc_result.asInt = 0;
		time_passed = 0;
		goto stats;
	}
	
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
	
	// we always compute the CRC, because it's nearly "for free",
	// since the file has to be read anyway.
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
	
	time_passed = mspassed(&starttime);
	
	stats:
	ulz_snprintf(crc_str, sizeof(crc_str), "%.8x", htonl(crc_result.asInt));
	
	// we do not use printf because it has a limited buffer size
	log_put(1, VARISL("CRC: "), VARIC(crc_str), VARISL(", "), 
		VARIS(src), VARISL(" -> "), VARIS(dst), 
		VARISL(" @"), VARIC(getMbsString(mbs_str, sizeof(mbs_str), src_stat->st_size, time_passed)),
		VARISL(" ("), VARIC(reason), VARISL(")"),
		NULL);
	
	progstate.total.copied += src_stat->st_size;
	progstate.total.copies += 1;
}

static void doFile(stringptr* src, stringptr* dst, stringptr* diff, struct stat* ss) {
	struct stat sd;
	char* reason = "x";
	if(stat(dst->ptr, &sd) == -1) {
		switch(errno) {
			case ENOENT:
				if(progstate.checkExists) {
					reason = "e";
					do_sync:
					doSync(src, diff, ss, reason);
				}
				return;
			default:
				log_puts(2, dst);
				log_puts(2, SPL(" "));
				log_perror("stat dest");
				break;
		}
	}
	if(progstate.checkFileSize && ss->st_size != sd.st_size) {
		reason = "f";
		goto do_sync;
	} else if (progstate.checkDate && ss->st_mtime > sd.st_mtime) {
		reason = "d";
		goto do_sync;
	} else if (progstate.checkDateOlder && ss->st_mtime < sd.st_mtime) {
		reason = "o";
		goto do_sync;
	} else if (!progstate.checkDateOlder && ss->st_mtime < sd.st_mtime) {
		ulz_fprintf(2, "dest is newer than source: %s , %s : %llu , %llu\n", src->ptr, dst->ptr, ss->st_mtime, sd.st_mtime);
	} else if(progstate.checkChecksum) {
		/* TODO launch 2 processes, each computing the CRC of src/dest in parallel, 
		 * then join em and compare crc and warn and copy if different
		 */
	}
	progstate.total.skipped += 1;
}

static void setLinkTimestamp(stringptr* link, struct stat* ss) {
	struct timeval tv[2];
	tv[0].tv_sec  = ss->st_atime;
	tv[0].tv_usec = ss->st_atim.tv_nsec / 1000;
	tv[1].tv_sec  = ss->st_mtime;
	tv[1].tv_usec = ss->st_mtim.tv_nsec / 1000;
	if(lutimes(link->ptr, tv) == -1) {
		log_perror("lutimes");
	}
}

// FIXME dont copy symlink if the target is equal
// FIXME dont increment progstate.total.symlink in case of failure
static void doLink(stringptr* src, stringptr* dst, struct stat* ss) {
	char buf[4096 + 1];
	int wasdir = 0;
	struct stat sd;
	ssize_t ret;
	if(progstate.simulate) goto skip;
	
	ret = readlink(src->ptr, buf, sizeof(buf) - 1);
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
	
	wasdir = removeTrailingSlash(dst);
	
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
	
	setLinkTimestamp(dst, ss);
	
	if(wasdir) 
		restoreTrailingSlash(dst);
	skip:
	progstate.total.symlink += 1;
}

static void doDir(stringptr* subd) {
	filelist f;
	stringptr *combined_src = stringptr_concat(progstate.srcdir, subd, NULL);
	stringptr *combined_dst = stringptr_concat(progstate.dstdir, subd, NULL);
	stringptr *combined_diff = stringptr_concat(progstate.diffdir, subd, NULL);
	
	struct stat src_stat;
	
	if(!filelist_search(&f, combined_src, SPL("*"), FLF_EXCLUDE_PATH | FLF_INCLUDE_HIDDEN)) {
		stringptr* file;
		stringptr* file_combined_src;
		stringptr* file_combined_dst;
		stringptr* file_combined_diff;
		sblist_iter(f.files, file) {
			file_combined_src = stringptr_concat(combined_src, file, NULL);
			file_combined_dst = stringptr_concat(combined_dst, file, NULL);
			file_combined_diff = stringptr_concat(combined_diff, file, NULL);
			
			removeTrailingSlash(file_combined_src); // remove trailing slash so stat doesnt resolve symlinks...
			
			if(lstat(file_combined_src->ptr, &src_stat) == -1) {
				log_puts(2, file_combined_src);
				log_puts(2, SPL(" "));
				log_perror("stat");
			} else {
				if(S_ISLNK(src_stat.st_mode)) {
					
					doLink(file_combined_src, file_combined_diff, &src_stat);
					
				} else if(isdir(file)) {
					restoreTrailingSlash(file_combined_src);

					stringptr *path_combined = stringptr_concat(subd, file, NULL);
					if(!progstate.simulate && access(file_combined_diff->ptr, R_OK) == -1 && errno == ENOENT) {
						makeDir(file_combined_diff, &src_stat);
					}
					// else updateTimestamp(file_combined_dst, &src_stat);
					doDir(path_combined);
					stringptr_free(path_combined);
					if(!progstate.simulate)
						updateTimestamp(file_combined_diff, &src_stat);
				} else {
					doFile(file_combined_src, file_combined_dst, file_combined_diff, &src_stat);
				}
			}
			stringptr_free(file_combined_src);
			stringptr_free(file_combined_dst);
			stringptr_free(file_combined_diff);
		}
		filelist_free(&f);
	}
	
	stringptr_free(combined_src);
	stringptr_free(combined_dst);
	stringptr_free(combined_diff);
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
	log_puts(1, SPL("filesync OPTIONS srcdir dstdir [diffdir]\n\n"
		"if diffdir is given, the program will check for files in destdir,\n"
		"but will write into diffdir instead. this allows usage as a simple\n"
		"incremental backup tool.\n\n"
		"\toptions: -s[imulate] -e[xists] -d[ate] -o[lder] -f[ilesize] -c[hecksum]\n"
		"\t-s  : only simulate and print to stdout (dry run)\n"
		"\t      note: will not print symlinks currently\n"
		"\t-e  : copy source files that dont exist on the dest side\n"
		"\t-d  : copy source files with newer timestamp (modtime)\n"
		"\t-o  : copy source files with older timestamp (modtime)\n"
		"\t-f  : copy source files with different filesize\n"
		"\t-c  : copy source files if checksum is different (not implemented yet)\n\n"
		"WARNING: you should *always* redirect stdout and stderr\n"
		"into some logfile. to see the actual state, attach with\n"
		"tail -f or tee...\n"
		"After a full run you can pipe the stdout.txt into the supplied\n"
		"perl script which can check the CRCs, in case you want to\n"
		"verify the copy. it is proposed that this run happens separately,\n"
		"so that the copied files are no longer buffered.\n\n"
	));
	return 1;
}

int main (int argc, char** argv) {
	
	if(argc < 4) return syntax();
	int startarg = 1;
	int freedst = 0, freediff = 0;
	int dirargs = 0, i;
	struct timeval starttime;
	struct stat src_stat;
	
	op_state op_b, *op = &op_b;
	
	op_init(op, argc, argv);

	progstate.simulate = op_hasflag(op, SPL("s")) || op_hasflag(op, SPL("simulate"));
	
	progstate.checkExists = op_hasflag(op, SPL("e")) || op_hasflag(op, SPL("exists"));
	progstate.checkFileSize = op_hasflag(op, SPL("f")) || op_hasflag(op, SPL("filesize"));
	progstate.checkDate = op_hasflag(op, SPL("d")) || op_hasflag(op, SPL("date"));
	progstate.checkDateOlder = op_hasflag(op, SPL("o")) || op_hasflag(op, SPL("older"));
	progstate.checkChecksum = op_hasflag(op, SPL("c")) || op_hasflag(op, SPL("checksum"));
	
	for(i = 1; i < argc; i++)
		if(argv[i][0] != '-') dirargs++;
	
	if(dirargs < 2 || dirargs > 3) {
		log_puts(2, SPL("invalid arguments detected\n"));
		return syntax();
	}
	
	startarg = argc - dirargs;
	
	memset(&progstate.total, 0, sizeof(totals));
	
	progstate.srcdir = stringptr_fromchar(argv[startarg], &progstate.srcdir_b);
	progstate.dstdir = stringptr_fromchar(argv[startarg+1], &progstate.dstdir_b);
	if(dirargs == 3)
		progstate.diffdir = stringptr_fromchar(argv[startarg+2], &progstate.diffdir_b);
	else
		progstate.diffdir = stringptr_fromchar(argv[startarg+1], &progstate.diffdir_b);
	
	if(access(progstate.diffdir->ptr, R_OK) == -1) {
		if(errno == ENOENT) {
			if(stat(progstate.srcdir->ptr, &src_stat) == -1) {
				log_perror("stat");
				return 1;
			}
			makeDir(progstate.diffdir, &src_stat);
		} else {
			log_perror("uncaught error while trying to access dest/diff dir");
			return 1;
		}
	}
	
	if(!isdir(progstate.dstdir)) {
		progstate.dstdir = stringptr_concat(progstate.dstdir, SPL("/"), NULL);
		freedst = 1;
	}
	
	if(!isdir(progstate.diffdir)) {
		progstate.diffdir = stringptr_concat(progstate.diffdir, SPL("/"), NULL);
		freediff = 1;
	}
	
	gettimestamp(&starttime);
	
	CRC32C_InitTables();
	
	doDir(isdir(progstate.srcdir) ? SPL("") : SPL("/"));
	
	printStats(mspassed(&starttime));
	
	if(freedst) stringptr_free(progstate.dstdir);
	if(freediff) stringptr_free(progstate.diffdir);
	
	return 0;
}
