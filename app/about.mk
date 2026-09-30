< /$objtype/mkfile

TARG=rill-about
GEN=build/ziran/about-plan9
OFILES=`{ls $GEN/*.c | sed 's@\.c$@.'$O'@'}
HEADERS=`{ls $GEN/*.h}
BIN=/$objtype/bin
OUT=build/rill-about.$O.out
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
