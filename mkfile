</$objtype/mkfile

BIN=$home/bin/$objtype
RCBIN=/rc/bin

TARG=claude9fs

COMMONO=claude.$O patch.$O json.$O

claude9fs:V: claude9fs.$O $COMMONO
	$LD $LDFLAGS -o $target claude9fs.$O $COMMONO

%.$O: %.c
	$CC $CFLAGS $stem.c

clean:V:
	rm -f *.[$OS] claude9fs

install:V: claude9fs
	cp claude9fs $BIN/
	cp claudetalk $RCBIN/
	cp claudebox $RCBIN/

nuke:V: clean
	rm -f $BIN/claude9fs $RCBIN/claudetalk $RCBIN/claudebox

rebuild:V: clean claude9fs
	echo rebuilt
