< /$objtype/mkfile

# Host-generated current Ziran application and Kryon, compiled by native 8c.
TARG=rill-applications
GEN=build/ziran/applications-plan9
OFILES=`{ls $GEN/*.c | sed 's@\.c$@.'$O'@'}
HEADERS=`{ls $GEN/*.h}
BIN=/$objtype/bin
OUT=build/rill-applications.$O.out
CFLAGS=-FTVw
LIB=/$objtype/lib/libdraw.a /$objtype/lib/libmemdraw.a /$objtype/lib/libthread.a

all:V: $OUT

install:V: $BIN/$TARG

$GEN/%.$O: $GEN/%.c $HEADERS
	$CC $CFLAGS -I$GEN -o $target -c $GEN/$stem^.c

$OUT: $OFILES $LIB
	$LD -o $target $prereq

$BIN/$TARG: $OUT
	cp $OUT $BIN/$TARG
