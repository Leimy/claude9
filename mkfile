</$objtype/mkfile

BIN=$home/bin/$objtype
RCBIN=/rc/bin
SKILLSDIR=$home/lib/claude9/skills

TARG=claude9fs claudegraph

COMMONO=claude.$O json.$O

all:V: claude9fs claudegraph

claude9fs: claude9fs.$O $COMMONO
	$LD $LDFLAGS -o $target claude9fs.$O $COMMONO

claudegraph: claudegraph.$O
	$LD $LDFLAGS -o $target claudegraph.$O

%.$O: %.c
	$CC $CFLAGS $stem.c

claude9fs.$O claude.$O json.$O: claude.h json.h

clean:V:
	rm -f *.[$OS] claude9fs claudegraph

install:V: claude9fs claudegraph
	cp claude9fs $BIN/
	cp claudegraph $BIN/
	cp claudetalk $RCBIN/
	chmod +x $RCBIN/claudetalk
	mkdir -p $SKILLSDIR
	for(f in skills/*) cp $f $SKILLSDIR/

install_test:V: claude9fs claudegraph
	cp claude9fs $BIN/claude9fs_test
	cp claudegraph $BIN/claudegraph_test
	cp claudetalk_test $RCBIN/claudetalk_test
	chmod +x $RCBIN/claudetalk
	mkdir -p $SKILLSDIR
	for(f in skills/*) cp $f $SKILLSDIR/

nuke:V: clean
	rm -f $BIN/claude9fs $BIN/claudegraph $RCBIN/claudetalk
	rm -rf $home/lib/claude9
