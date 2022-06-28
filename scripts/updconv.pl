#!/usr/bin/perl

my $inn = $ARGV[0];

if ($inn eq "") {
	print STDERR "You must specify a source file to read\n";
	exit 1;
}

unless(open(IN,"<$inn")) {
	print STDERR "Cannot open $inn\n";
	exit 1;
}

my $binary = "";
my $line,$blen;

while ($line = <IN>) {
	chomp $line;
	$line =~ s/^ +//g;
	$line =~ s/ +$//g;

	if ($line =~ m/^-+/) {
		$blen = length($binary);
		print STDERR "total length = $blen\n";
		print $binary;
		$binary = "";
	}
	else {
		$line =~ s/[^0-9a-zA-Z]//g;
		my $t = pack("H*",$line);
		$binary .= $t;
		print STDERR "length = " . length($t) . "\n";
	}
}

close(IN);

