< /$objtype/mkfile

TARG=rill
KRYON=/sys/src/kryon
KTREM=/sys/src/ktrem
SHELF=/sys/src/shelf
BIN=/$objtype/bin
OUT=$O.out

CPPFLAGS=-I../include -I$KRYON/src/platform/plan9/include -I$KRYON/include \
	-I$KTREM/src -I$SHELF/src \
	-DKRYON_BACKEND_LIBDRAW -DKRYON_PLATFORM_PLAN9 -DKRYON_NATIVE_PLAN9
KTERMFLAGS=-DKTREM_PLAN9_EMBEDDED_HOST
CFLAGS=-FTVw

OFILES=\
	src/main.$O\
	src/rill_shell.$O\
	src/platform_plan9.$O\
	$KTREM/src/app_chrome.$O\
	$KTREM/src/app_clipboard.$O\
	$KTREM/src/app_commands.$O\
	$KTREM/src/app_context_menu.$O\
	$KTREM/src/app_input.$O\
	$KTREM/src/app_menu.$O\
	$KTREM/src/app_profile.$O\
	$KTREM/src/app_search.$O\
	$KTREM/src/app_sessions.$O\
	$KTREM/src/app_terminal_view.$O\
	$KTREM/src/config.$O\
	$KTREM/src/input.$O\
	$KTREM/src/ktrem_host.$O\
	$KTREM/src/launch_options.$O\
	$KTREM/src/palette.$O\
	$KTREM/src/profile.$O\
	$KTREM/src/selection.$O\
	$KTREM/src/session.$O\
	$KTREM/src/session_store.$O\
	$KTREM/src/terminal.$O\
	$KTREM/src/terminal_csi.$O\
	$KTREM/src/terminal_dcs.$O\
	$KTREM/src/terminal_keys.$O\
	$KTREM/src/terminal_modes.$O\
	$KTREM/src/terminal_mouse.$O\
	$KTREM/src/terminal_osc.$O\
	$KTREM/src/terminal_parser.$O\
	$KTREM/src/terminal_paste.$O\
	$KTREM/src/terminal_pty_plan9.$O\
	$KTREM/src/terminal_screen.$O\
	$KTREM/src/terminal_search.$O\
	$KTREM/src/terminal_sgr.$O\
	$KTREM/src/terminal_sixel.$O\
	$KTREM/src/terminal_text.$O\
	$KTREM/src/terminal_view.$O\
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
	rm -f src/*.[$OS] src/*.i [$OS].out $TARG $KTREM/src/*.[$OS] \
		$KTREM/src/*.i $SHELF/src/*.[$OS] $SHELF/src/*.i

$KTREM/src/ktrem_host.$O: $KTREM/src/ktrem_host.c
	cd $KTREM/src && cpp -+ $CPPFLAGS $KTERMFLAGS '-DCreateAppHost=KtermCreateAppHost' '-DDestroyAppHost=KtermDestroyAppHost' ktrem_host.c > ktrem_host.i && $CC $CFLAGS -c ktrem_host.i && mv ktrem_host.i.$O ktrem_host.$O && rm -f ktrem_host.i

$KTREM/src/%.$O: $KTREM/src/%.c
	cd $KTREM/src && cpp -+ $CPPFLAGS $KTERMFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

$SHELF/src/shelf_host.$O: $SHELF/src/shelf_host.c
	cd $SHELF/src && cpp -+ $CPPFLAGS '-DCreateAppHost=ShelfCreateAppHost' '-DDestroyAppHost=ShelfDestroyAppHost' shelf_host.c > shelf_host.i && $CC $CFLAGS -c shelf_host.i && mv shelf_host.i.$O shelf_host.$O && rm -f shelf_host.i

$SHELF/src/%.$O: $SHELF/src/%.c
	cd $SHELF/src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i
