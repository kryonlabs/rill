< /$objtype/mkfile

TARG=rill
KRYON=/sys/src/kryon
T9=/sys/src/t9
SHELF=/sys/src/shelf
BIN=/$objtype/bin
OUT=$O.out

CPPFLAGS=-I../include -I$KRYON/src/platform/plan9/include -I$KRYON/include \
	-I$KRYON/build/plan9/generated \
	-I$T9/src -I$SHELF/src \
	-DKRYON_BACKEND_LIBDRAW -DKRYON_PLATFORM_PLAN9 -DKRYON_NATIVE_PLAN9
T9FLAGS=-DT9_PLAN9_EMBEDDED_HOST
CFLAGS=-FTVw

OFILES=\
	src/main.$O\
	src/rill_shell.$O\
	src/rill_panel.$O\
	src/rill_settings.$O\
	src/platform_plan9.$O\
	$T9/src/app_chrome.$O\
	$T9/src/app_clipboard.$O\
	$T9/src/app_commands.$O\
	$T9/src/app_context_menu.$O\
	$T9/src/app_input.$O\
	$T9/src/app_menu.$O\
	$T9/src/app_profile.$O\
	$T9/src/app_search.$O\
	$T9/src/app_sessions.$O\
	$T9/src/app_terminal_view.$O\
	$T9/src/config.$O\
	$T9/src/input.$O\
	$T9/src/ktrem_host.$O\
	$T9/src/launch_options.$O\
	$T9/src/palette.$O\
	$T9/src/profile.$O\
	$T9/src/selection.$O\
	$T9/src/session.$O\
	$T9/src/session_store.$O\
	$T9/src/terminal.$O\
	$T9/src/terminal_csi.$O\
	$T9/src/terminal_dcs.$O\
	$T9/src/terminal_keys.$O\
	$T9/src/terminal_modes.$O\
	$T9/src/terminal_mouse.$O\
	$T9/src/terminal_osc.$O\
	$T9/src/terminal_parser.$O\
	$T9/src/terminal_paste.$O\
	$T9/src/terminal_pty_plan9.$O\
	$T9/src/terminal_screen.$O\
	$T9/src/terminal_search.$O\
	$T9/src/terminal_sgr.$O\
	$T9/src/terminal_sixel.$O\
	$T9/src/terminal_text.$O\
	$T9/src/terminal_view.$O\
	$SHELF/src/shelf.$O\
	$SHELF/src/shelf_host.$O\

LIB=/$objtype/lib/libkryon.a /$objtype/lib/libstdio.a

all:V: $OUT

install:V: $BIN/$TARG

$BIN/$TARG: $OUT
	cp $OUT $BIN/$TARG

$OUT: $OFILES $LIB
	$LD -o $target $prereq -ldraw -lmemdraw -lthread

src/%.$O: src/%.c
	cd src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

clean:V:
	rm -f src/*.[$OS] src/*.i [$OS].out $TARG $T9/src/*.[$OS] \
		$T9/src/*.i $SHELF/src/*.[$OS] $SHELF/src/*.i

$T9/src/ktrem_host.$O: $T9/src/ktrem_host.c
	cd $T9/src && cpp -+ $CPPFLAGS $T9FLAGS '-DCreateAppHost=T9CreateAppHost' '-DDestroyAppHost=T9DestroyAppHost' ktrem_host.c > ktrem_host.i && $CC $CFLAGS -c ktrem_host.i && mv ktrem_host.i.$O ktrem_host.$O && rm -f ktrem_host.i

$T9/src/%.$O: $T9/src/%.c
	cd $T9/src && cpp -+ $CPPFLAGS $T9FLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

$SHELF/src/shelf_host.$O: $SHELF/src/shelf_host.c
	cd $SHELF/src && cpp -+ $CPPFLAGS '-DCreateAppHost=ShelfCreateAppHost' '-DDestroyAppHost=ShelfDestroyAppHost' shelf_host.c > shelf_host.i && $CC $CFLAGS -c shelf_host.i && mv shelf_host.i.$O shelf_host.$O && rm -f shelf_host.i

$SHELF/src/%.$O: $SHELF/src/%.c
	cd $SHELF/src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

test:V: tests/rill_plan9_test.$O src/rill_panel.$O src/rill_settings.$O
	$LD -o rill-test.$O.out $prereq /$objtype/lib/libstdio.a
	./rill-test.$O.out

tests/%.$O: tests/%.c
	cd tests && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i
