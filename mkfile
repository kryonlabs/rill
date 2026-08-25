</$objtype/mkfile

BIN=/$objtype/bin/rill
ROOT=/sys/src/rill
KRYON=/sys/src/kryon
SHIM=$KRYON/src/platform/plan9/include

CPPFLAGS=-I$SHIM -I$ROOT/include -I$KRYON/include \
	-DKRYON_BACKEND_LIBDRAW -DKRYON_PLATFORM_PLAN9 -DKRYON_NATIVE_PLAN9
CFLAGS=-FTVw

OFILES=\
	src/main.$O\
	src/rill_shell.$O\
	src/platform_plan9.$O\

all:V: $BIN

install:V: $BIN

$BIN: $OFILES
	$LD -o $target $OFILES -lkryon -ldraw -lmemdraw -lthread

clean:V:
	rm -f src/*.$O src/*.i

src/%.$O: src/%.c
	cd src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i
