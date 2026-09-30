< /$objtype/mkfile

TARG=rill
KRYON=/sys/src/kryon
T9=/sys/src/t9
SHELF=/sys/src/shelf
BIN=/$objtype/bin
OUT=$O.out
RILLGEN=build/ziran/plan9
RILLOBJS=$RILLGEN/native_memory.$O $RILLGEN/platform_types.$O \
	$RILLGEN/shell_types.$O $RILLGEN/shell.$O $RILLGEN/run_dialog.$O $RILLGEN/applications.$O \
	$RILLGEN/c_string.$O $RILLGEN/file_plan9.$O $RILLGEN/native_files.$O \
	$RILLGEN/panel_types.$O $RILLGEN/panel.$O $RILLGEN/settings.$O \
	$RILLGEN/platform_plan9.$O $RILLGEN/process_plan9.$O \
	$RILLGEN/number_text.$O $RILLGEN/text_buffer.$O
RILLTEST=build/ziran/plan9-test
RILLTESTOBJS=$RILLTEST/native_memory.$O $RILLTEST/platform_types.$O \
	$RILLTEST/c_string.$O $RILLTEST/file_plan9.$O $RILLTEST/native_files.$O \
	$RILLTEST/panel_types.$O $RILLTEST/panel.$O $RILLTEST/settings.$O \
	$RILLTEST/persistence_test.$O

# t9 is authored in .kry and emitted ahead of time on the host
# t9 is authored in .kry and emitted ahead of time on the host
# (`make kry-c-plan9` in /sys/src/t9). Rill builds those generated
# objects with the embedded-host include contract and compiles only the
# two files that need symbol renaming: the app host entry and PTY boundary.
T9GEN=$T9/build/plan9/generated
T9LIST=$T9/build/plan9/generated-c-files.txt
T9OBJ=$T9/build/plan9/obj
T9GENOBJS=`{cat $T9LIST | grep -v -e 'engine/process.c' -e 'src/app/app_ktrem_host.c' | sed -e 's@\.c$@.8@' -e 's@^@'$T9OBJ'/@'}

CPPFLAGS=-I../include -I$KRYON/src/platform/plan9/include -I$KRYON/include \
	-I$KRYON/build/plan9/generated \
	-I$T9/src -I$SHELF/src \
	-DKRYON_BACKEND_LIBDRAW -DKRYON_PLATFORM_PLAN9 -DKRYON_NATIVE_PLAN9
T9CPPFLAGS=-I$KRYON/src/platform/plan9/include -I$KRYON/include -I$KRYON/src \
	-I$KRYON/src/ui -I$KRYON/build/plan9/generated \
	-I$KRYON/build/plan9/generated/runtime -I$KRYON/build/plan9/generated/src \
	-I$T9/src -I$T9GEN/src -I$T9GEN \
	-DKRYON_BACKEND_LIBDRAW=1 -DKRYON_PLATFORM_PLAN9=1 -DKRYON_NATIVE_PLAN9=1 \
	-DKRYON_EMBEDDED_ONLY=1 -DT9_PLAN9_EMBEDDED_HOST
CFLAGS=-FTVw

OFILES=\
	src/main.$O\
	$RILLOBJS\
	t9_host.$O\
	t9_pty.$O\
	$T9GENOBJS\
	$SHELF/src/shelf.$O\
	$SHELF/src/shelf_host.$O\

LIB=/$objtype/lib/libkryon.a /$objtype/lib/libstdio.a

all:V: check-rill-ziran check-t9 $OUT

install:V: check-rill-ziran check-t9 $BIN/$TARG

check-rill-ziran:V:
	if(! test -f $RILLGEN/shell.c || ! test -f $RILLGEN/panel.c || ! test -f $RILLGEN/settings.c || ! test -f $RILLGEN/platform_plan9.c) {
		echo 'missing Ziran desktop output; run make ziran-c-plan9 on the host' >[1=2]
		exit missing
	}
	exit 0

$RILLGEN/%.$O: $RILLGEN/%.c
	$CC $CFLAGS -I$RILLGEN -o $target -c $prereq

$RILLTEST/%.$O: $RILLTEST/%.c
	$CC $CFLAGS -I$RILLTEST -o $target -c $prereq

check-t9:V:
	if(! test -f $T9LIST) {
		echo 'missing '^$T9LIST^'; run make kry-c-plan9 in /sys/src/t9 on the host' >[1=2]
		exit missing
	}
	exit 0

$BIN/$TARG: $OUT
	cp $OUT $BIN/$TARG

$OUT: $OFILES $LIB
	$LD -o $target $prereq -ldraw -lmemdraw -lthread

src/%.$O: src/%.c
	cd src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

clean:V:
	rm -f src/*.[$OS] src/*.i [$OS].out $TARG t9_host.*[$OS] t9_host.i \
		t9_pty.*[$OS] t9_pty.i $SHELF/src/*.[$OS] $SHELF/src/*.i $RILLGEN/*.[$OS] $RILLTEST/*.[$OS]

t9_host.$O: $T9GEN/src/app/app_ktrem_host.c
	cpp -+ $T9CPPFLAGS '-DCreateAppHost=T9CreateAppHost' '-DDestroyAppHost=T9DestroyAppHost' $prereq > t9_host.i && $CC $CFLAGS -c t9_host.i && mv t9_host.i.$O t9_host.$O && rm -f t9_host.i

t9_pty.$O: $T9/src/terminal_pty_plan9.c
	cpp -+ $T9CPPFLAGS $prereq > t9_pty.i && $CC $CFLAGS -c t9_pty.i && mv t9_pty.i.$O t9_pty.$O && rm -f t9_pty.i

$T9OBJ/%.8: $T9GEN/%.c
	mkdir -p `{echo $target | sed 's@/[^/]*$@@'} && cpp -+ $T9CPPFLAGS $prereq > $T9OBJ/$stem.i && $CC $CFLAGS -o $target -c $T9OBJ/$stem.i && rm -f $T9OBJ/$stem.i

$SHELF/src/shelf_host.$O: $SHELF/src/shelf_host.c
	cd $SHELF/src && cpp -+ $CPPFLAGS '-DCreateAppHost=ShelfCreateAppHost' '-DDestroyAppHost=ShelfDestroyAppHost' shelf_host.c > shelf_host.i && $CC $CFLAGS -c shelf_host.i && mv shelf_host.i.$O shelf_host.$O && rm -f shelf_host.i

$SHELF/src/%.$O: $SHELF/src/%.c
	cd $SHELF/src && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i

test:V: $RILLTESTOBJS
	$LD -o rill-test.$O.out $prereq
	testroot=/tmp/rill-persistence-test-$pid
	mkdir $testroot
	RILL_TEST_ROOT=$testroot ./rill-test.$O.out
	teststatus=$status
	rm -rf $testroot
	exit $teststatus

tests/%.$O: tests/%.c
	cd tests && cpp -+ $CPPFLAGS $stem.c > $stem.i && $CC $CFLAGS -c $stem.i && mv $stem.i.$O $stem.$O && rm -f $stem.i
