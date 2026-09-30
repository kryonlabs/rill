< /$objtype/mkfile

# Host-generated current Ziran application and Kryon, compiled by native 8c.
TARG=rill-run
GEN=build/ziran/run-plan9
OFILES=`{ls $GEN/*.c | sed 's@\.c$@.'$O'@'}
BIN=/$objtype/bin
OUT=build/rill-run.$O.out
CFLAGS=-FTVw
LIB=/$objtype/lib/libdraw.a /$objtype/lib/libmemdraw.a /$objtype/lib/libthread.a

all:V: $OUT

install:V: $BIN/$TARG

$GEN/%.$O: $GEN/%.c
	$CC $CFLAGS -I$GEN -o $target -c $prereq

$OUT: $OFILES $LIB
	$LD -o $target $prereq

$BIN/$TARG: $OUT
	cp $O.out $BIN/$TARG
