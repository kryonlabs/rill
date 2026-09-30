< /$objtype/mkfile

TARG=rill-open
GEN=build/ziran/open-plan9
OFILES=`{ls $GEN/*.c | sed 's@\.c$@.'$O'@'}
HEADERS=`{ls $GEN/*.h}
BIN=/$objtype/bin
OUT=build/rill-open.$O.out
CFLAGS=-FTVw

all:V: $OUT

install:V: $BIN/$TARG

$GEN/%.$O: $GEN/%.c $HEADERS
	$CC $CFLAGS -I$GEN -o $target -c $GEN/$stem^.c

$OUT: $OFILES
	$LD -o $target $prereq

$BIN/$TARG: $OUT
	cp $OUT $BIN/$TARG
