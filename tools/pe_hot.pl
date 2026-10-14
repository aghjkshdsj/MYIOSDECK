#!/usr/bin/perl
# Names the code around hot addresses of a 64-bit PE (the fxi-prof "hot" lines of a device log
# give module+RVA): for each RVA, the function that holds it (from the exception directory), its
# disassembly, the strings it references, and who calls it, a few levels up. Runs on the CI
# runner with GNU objdump; nothing is written next to the PE.
#   tools/pe_hot.pl <file.dll> <rva> [<rva> ...]     (RVAs in hex, e.g. 0x54d2d)
use strict;
use warnings;
no warnings "portable";   # 64-bit addresses through hex()

my ($pe, @rvas) = @ARGV;
die "usage: $0 file.dll rva...\n" unless $pe && @rvas;
open(my $fh, '<:raw', $pe) or die "$pe: $!\n";
my $img = do { local $/; <$fh> };
close $fh;

my $lfanew = unpack('V', substr($img, 0x3c, 4));
die "not PE\n" unless substr($img, $lfanew, 4) eq "PE\0\0";
my ($machine, $nsect, undef, undef, undef, $optsize) = unpack('vvVVVv', substr($img, $lfanew + 4, 20));
my $opt = $lfanew + 24;
die "not PE32+\n" unless unpack('v', substr($img, $opt, 2)) == 0x20b;
my $base = unpack('Q<', substr($img, $opt + 24, 8));
my ($exc_rva, $exc_size) = unpack('VV', substr($img, $opt + 112 + 3 * 8, 8));
my @sect;
for my $i (0 .. $nsect - 1) {
    my $s = $opt + $optsize + $i * 40;
    my ($name, $vsize, $va, $rawsize, $rawptr) = unpack('Z8VVVV', substr($img, $s, 24));
    my $chars = unpack('V', substr($img, $s + 36, 4));
    push @sect, { name => $name, va => $va, vsize => $vsize, raw => $rawptr, rawsize => $rawsize, chars => $chars };
}
sub off { my $r = shift; for (@sect) { return $_->{raw} + $r - $_->{va} if $r >= $_->{va} && $r < $_->{va} + $_->{rawsize} } undef }
sub sect_of { my $r = shift; for (@sect) { return $_ if $r >= $_->{va} && $r < $_->{va} + $_->{vsize} } undef }

# RUNTIME_FUNCTION entries; chained unwind info (flag 4) points at the primary entry.
my @funcs;
my $eo = off($exc_rva);
for (my $i = 0; $i + 12 <= $exc_size; $i += 12) {
    my ($b, $e, $u) = unpack('VVV', substr($img, $eo + $i, 12));
    push @funcs, [$b, $e, $u] if $b && $e > $b;
}
@funcs = sort { $a->[0] <=> $b->[0] } @funcs;
printf "%s: machine 0x%x, image base 0x%x, %d sections, %d function entries\n", $pe, $machine, $base, $nsect, scalar @funcs;

sub func_of {
    my $r = shift;
    my ($lo, $hi) = (0, $#funcs);
    while ($lo <= $hi) {
        my $m = int(($lo + $hi) / 2);
        if ($r < $funcs[$m][0]) { $hi = $m - 1 } elsif ($r >= $funcs[$m][1]) { $lo = $m + 1 } else { return $funcs[$m] }
    }
    return undef;
}
# The primary entry of a chained fragment (UNW_FLAG_CHAININFO): its unwind info ends with one.
sub primary {
    my $f = shift;
    for (1 .. 8) {
        my $uo = off($f->[2] & ~1) // return $f;
        my ($vf, undef, $count) = unpack('CCC', substr($img, $uo, 3));
        return $f unless ($vf >> 3) & 4;
        my $chain = $uo + 4 + 2 * ($count + ($count & 1));
        my ($b, $e, $u) = unpack('VVV', substr($img, $chain, 12));
        $f = [$b, $e, $u];
    }
    return $f;
}

sub cstring {
    my $r = shift;
    my $o = off($r) // return undef;
    my $s = sect_of($r) // return undef;
    return undef if $s->{chars} & 0x20000000;   # code
    my $str = substr($img, $o, 200);
    if ($str =~ /^([\x20-\x7e]{4,})\0/) { return $1 }
    if ($str =~ /^((?:[\x20-\x7e]\0){4,})\0\0/) { (my $w = $1) =~ s/\0//g; return "L\"$w\"" }
    return undef;
}

# Whole-image disassembly once, for callers.
my $dis = "/tmp/pe_hot.$$.txt";
system("objdump -d -M intel --no-show-raw-insn '$pe' > $dis") == 0 or die "objdump failed\n";
my %callers;   # target va -> [site va...]
open(my $d, '<', $dis) or die;
while (<$d>) {
    next unless /^\s*([0-9a-f]+):\s+(call|jmp)\s+(?:QWORD PTR \[rip\+0x[0-9a-f]+\]\s+#\s+)?(?:0x)?([0-9a-f]+)\b/;
    push @{$callers{hex $3}}, hex $1;
}
close $d;
my %leas;      # rip-relative references to a function (callbacks, vtables are not covered)
open($d, '<', $dis) or die;
while (<$d>) {
    next unless /^\s*([0-9a-f]+):\s+lea\s+\S+,\[rip\+0x[0-9a-f]+\]\s+#\s+(?:0x)?([0-9a-f]+)/;
    push @{$leas{hex $2}}, hex $1;
}
close $d;

my %shown;
sub show {
    my ($rva, $depth, $why) = @_;
    my $f = func_of($rva);
    unless ($f) { printf "%s  rva 0x%x: no function entry (leaf code without unwind info)\n", '  ' x $depth, $rva; return }
    $f = primary($f);
    my ($b, $e) = @$f;
    my $pad = '  ' x $depth;
    printf "%s== function rva 0x%x..0x%x (%d bytes) holds 0x%x%s\n", $pad, $b, $e, $e - $b, $rva, $why;
    return if $shown{$b}++;
    my $start = sprintf('0x%x', $base + $b);
    my $stop = sprintf('0x%x', $base + $e);
    my @lines = `objdump -d -M intel --no-show-raw-insn --start-address=$start --stop-address=$stop '$pe'`;
    @lines = grep { /^\s*[0-9a-f]+:/ } @lines;
    my %strs;
    for (@lines) {
        $strs{$1} = 1 while /#\s+(?:0x)?([0-9a-f]{6,})/g;
    }
    my @named = grep { defined } map { my $s = cstring(hex($_) - $base); $s ? sprintf('0x%x "%s"', hex($_) - $base, $s) : undef } sort keys %strs;
    print "$pad   strings: $_\n" for @named[0 .. ($#named < 24 ? $#named : 24)];
    my $limit = $depth == 0 ? 400 : 60;
    print "$pad   | $_" for @lines[0 .. ($#lines < $limit ? $#lines : $limit)];
    print "$pad   | ... (" . scalar(@lines) . " instructions)\n" if @lines > $limit + 1;
    return if $depth >= 4;
    my @sites = @{$callers{$base + $b} // []};
    my @refs = @{$leas{$base + $b} // []};
    printf "%s   called from %d sites, address taken at %d\n", $pad, scalar @sites, scalar @refs;
    my $n = 0;
    for my $site (@sites, @refs) {
        last if ++$n > 5;
        show($site - $base, $depth + 1, sprintf(' (%s at 0x%x)', (grep { $_ == $site } @sites) ? 'call' : 'lea', $site - $base));
    }
}
for my $r (@rvas) {
    my $rva = hex($r =~ s/^0x//r);
    print "\n######## hot rva 0x" . sprintf('%x', $rva) . "\n";
    show($rva, 0, '');
}
unlink $dis;
