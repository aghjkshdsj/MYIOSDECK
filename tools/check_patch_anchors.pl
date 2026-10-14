#!/usr/bin/perl
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks an engine/*/patches/*.py patch without Python: emulates its apply() for one list of
# (anchor, replacement) pairs, NAME = [ (old, new), ... ] of Python string literals and named
# string constants, applied in order to a copy of the target file (CRLF read as LF). Prints each
# anchor's count (it must be 1) and, given an out file, writes the patched result there.
# Usage: tools/check_patch_anchors.pl <patch.py> <LISTNAME> <target file> [out file]
# e.g.   tools/check_patch_anchors.pl engine/wine/patches/session_reuse.py SELF_EXIT_FIXES wine/server/thread.c
use strict; use warnings;
my ($py, $list, $target, $out) = @ARGV;
my $src = do { local $/; open my $f, '<', $py or die "$py: $!"; <$f> };
my %const;

sub unesc {
    my ($s) = @_;
    $s =~ s/\\(n|t|\\|"|'|\n)/$1 eq 'n' ? "\n" : $1 eq 't' ? "\t" : $1 eq "\n" ? '' : $1/ge;
    return $s;
}

# tokens of an expression: strings, names, punctuation
sub tokens {
    my ($t) = @_;
    my @tok;
    pos($t) = 0;
    while (pos($t) < length $t) {
        if ($t =~ /\G\s+/gc) { next }
        if ($t =~ /\G#[^\n]*/gc) { next }
        if ($t =~ /\G([rR]?)'''(.*?)'''/gcs) { push @tok, ['S', $1 ? $2 : unesc($2)]; next }
        if ($t =~ /\G([rR]?)"""(.*?)"""/gcs) { push @tok, ['S', $1 ? $2 : unesc($2)]; next }
        if ($t =~ /\G([rR]?)'((?:[^'\\\n]|\\.)*)'/gc) { push @tok, ['S', $1 ? $2 : unesc($2)]; next }
        if ($t =~ /\G([rR]?)"((?:[^"\\\n]|\\.)*)"/gc) { push @tok, ['S', $1 ? $2 : unesc($2)]; next }
        if ($t =~ /\G([A-Za-z_]\w*)/gc) { push @tok, ['N', $1]; next }
        if ($t =~ /\G([()\[\],+])/gc) { push @tok, ['P', $1]; next }
        die "cannot tokenize at: " . substr($t, pos($t), 60);
    }
    return @tok;
}

# value of a sequence of tokens: adjacent strings and names joined (implicit or +)
sub value {
    my $v = '';
    for my $k (@_) {
        if ($k->[0] eq 'S') { $v .= $k->[1] }
        elsif ($k->[0] eq 'N') { defined $const{$k->[1]} or die "unknown name $k->[1]"; $v .= $const{$k->[1]} }
    }
    return $v;
}

# top-level string constants: NAME = <string or name> [+ <string or name>]...
while ($src =~ /^([A-Z][A-Z0-9_]*) = (?!\[)/gm) {
    my $name = $1;
    my $rest = substr($src, pos($src));
    my @t; my $ok = 1;
    pos($rest) = 0;
    while (1) {
        $rest =~ /\G[ \t]*/gc;
        if ($rest =~ /\G([rR]?)'''(.*?)'''/gcs) { push @t, ['S', $1 ? $2 : unesc($2)] }
        elsif ($rest =~ /\G([rR]?)"((?:[^"\\\n]|\\.)*)"/gc) { push @t, ['S', $1 ? $2 : unesc($2)] }
        elsif ($rest =~ /\G([rR]?)'((?:[^'\\\n]|\\.)*)'/gc) { push @t, ['S', $1 ? $2 : unesc($2)] }
        elsif ($rest =~ /\G([A-Za-z_]\w*)/gc) { push @t, ['N', $1] }
        else { $ok = 0; last }
        $rest =~ /\G[ \t]*/gc;
        last unless $rest =~ /\G\+/gc;
    }
    next unless $ok && @t;
    my $v = eval { value(@t) };
    $const{$name} = $v unless $@;
}

$src =~ /^\Q$list\E = \[\n(.*?)^\]/ms or die "no list $list";
my @t = tokens($1);
my (@pairs, @cur, @elem, $depth);
$depth = 0;
for my $k (@t) {
    if ($k->[0] eq 'P' && $k->[1] eq '(') { if ($depth++ == 0) { @cur = (); @elem = (); next } }
    if ($k->[0] eq 'P' && $k->[1] eq ')') {
        if (--$depth == 0) { push @cur, value(@elem) if @elem; push @pairs, [@cur]; next }
    }
    next if $depth == 0;
    if ($depth == 1 && $k->[0] eq 'P' && $k->[1] eq ',') { push @cur, value(@elem); @elem = (); next }
    push @elem, $k unless $k->[0] eq 'P' && $k->[1] eq '+';
}

my $s = do { local $/; open my $f, "<", $target or die "$target: $!"; <$f> }; $s =~ s/\r\n/\n/g;
my $ok = 1;
for my $p (@pairs) {
    my ($old, $new) = @$p;
    my ($n, $i) = (0, -1);
    while (($i = index($s, $old, $i + 1)) >= 0) { $n++ }
    my $show = substr($old, 0, 70); $show =~ s/\n/\\n/g;
    printf "%d  %s\n", $n, $show;
    if ($n != 1) { $ok = 0; next }
    my $at = index($s, $old);
    substr($s, $at, length $old) = $new;
}
print $ok ? "ALL ANCHORS OK ($list, " . scalar(@pairs) . ")\n" : "ANCHOR FAILURE ($list)\n";
if ($out) { open my $o, '>', $out or die; print $o $s; close $o }
exit($ok ? 0 : 1);
