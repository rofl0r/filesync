#!/bin/sh
# Copyright (C) 2012  rofl0r

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# this program will use 2 directories for backups
# month0 and month1
# the current month is taken and modulo 2 applied. so for example
# if the month is march, 3 % 1 == 1, the backup will be placed in
# month1 directory.
# inside this directory, a file "backup.month" will be checked
# if the content is equal to the current month, 3 in our case,
# a normal "daily" backup will be created. if not, the contents of the
# directory will be trashed, and a "full" backup initiated.
# the full backup is an exact copy of the source folder, and will be placed
# in a directory called "full"
# the daily backup will compare the source folder with the full backup
# and put the differences inside a folder "dayXX", where XX is equivalent
# to the number of the day. i.e. 13 if launched on march 13th.
# the daily backup will contain all files changed or added since the last
# full backup.

# using this strategy will lead to an incremental backup covering at least
# one entire month, and maximum 2 months - 1 day.
# given a relatively static set of data, this should take 2-3 times the size
# of the source directory.

# if you want to cover more than 2 months, just change the variable 
# month_cycles accordingly (either 2,3,4,6 or 12 make sense).
# be aware that changing this variable will possibly overwrite existing 
# backups.

backup_source=/srv/
backup_root=/mnt/backup/
filesync_cmd=filesync
month_cycles=2

month=`date +%m`
day=`date +%d`
month_mod=`expr $month % $month_cycles`
backup_dir="${backup_root}/month${month_mod}/"
backup_full="${backup_dir}/full/"
backup_day="${backup_dir}/day${day}/"
backup_cfg="${backup_dir}/backup.month"
log_dir="${backup_dir}/logs/"

do_backup_full() {
	rm -rf "$backup_full"
	rm -rf "${backup_dir}/day*"
	rm -rf "${log_dir}/*"
	echo $month > "$backup_cfg"
	# we write to a logfile to prevent getting the stdout mailed by cron
	# which would mean megabytes per run on a big number of files
	# this way the logs will be cleared together with the data after
	# completion of a cycle.
	logfile="${log_dir}/full.log"
	"$filesync_cmd" -e -d -f "$backup_source" "$backup_full" > "$logfile"
}

do_backup_daily() {
	logfile="${log_dir}/day${day}.log"
	"$filesync_cmd" -e -d -f "$backup_source" "$backup_full" "$backup_day" > "$logfile"
}


mkdir -p "${backup_dir}" "${log_dir}" || (echo failed to create directory structure; exit 1)

if [ -e "$backup_cfg" ] ; then
	cont=`cat "$backup_cfg"`
	if [ "$cont" = "$month" ] ; then
		do_backup_daily
	else
		do_backup_full
	fi
else
	do_backup_full
fi
