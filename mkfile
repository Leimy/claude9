P9=$PLAN9

<$P9/src/mkhdr

TARG=claude9

OFILES=\
	chat.$O\
	claude.$O\
	json.$O\

HFILES=\
	claude.h\
	json.h\

BIN=$HOME/bin

<$P9/src/mkone
