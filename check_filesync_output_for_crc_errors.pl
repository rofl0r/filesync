#!/usr/bin/env perl
use strict;
use warnings;

while(<>) {
	if(/CRC: (\w{8}), (.*?) -> (.*?) @([\d\.]+ MB\/s)/) {
		my $crc = $1;
		my $newfile = $3;
		open my $chk_f, "-|", "./crc_check.out", $newfile;
		my $chk = <$chk_f>;
		chomp $chk;
		close($chk_f);
		print $newfile, ": CRC mismatch $crc, got $chk\n" if $chk ne $crc;
	}
}
