/*
 * claudegraph - live 3D view of claude9fs sessions and their
 * sub-agent relationships, in the spirit of ubigraph/vacuum
 * (the old GHC heap visualizers): a force-directed graph of
 * glowing spheres floating in dark space, gently rotating,
 * with edges showing who spawned whom.
 *
 * Data model
 * ----------
 * Each claude9fs mount exposes a "graphlive" file: one
 * tab-separated line per live session,
 *
 *	name  model  busy  parent  idlesecs
 *
 * "parent" is just a label a session's ctl was told
 * ("echo parent <name> > ctl") -- typically written by an
 * orchestrating session right after cloning a sub-agent, per
 * the claude9 sub-agent skill.  Note that in the standard
 * claudetalk setup the orchestrator lives on one mount
 * (/mnt/claude) and its sub-agents on another (/mnt/claudesub),
 * and the parent label names a session on the *other* server;
 * edges are therefore resolved by name across all sources, not
 * just within one (preferring a same-source match when names
 * collide).  "idlesecs" is how long the session has been idle
 * as of the snapshot (0 while busy); the viewer adds its own
 * clock time since the snapshot so idle ages stay live without
 * the server having to bump the graph as time passes.
 *
 * Sessions are never garbage-collected by claude9fs: they live
 * until an explicit "hangup" ctl write.  A sub-agent someone
 * forgot to hang up therefore stays in the graph forever.  The
 * viewer makes such sessions obvious instead of eternal-looking:
 * a session idle for more than a couple of minutes fades toward
 * the background, and button 3 over a node pops a menu whose
 * "hangup" entry writes the hangup for you.
 *
 * Rendering
 * ---------
 * Sessions are laid out by a little 3D spring embedder: edges
 * pull parent and child together, every pair of nodes repels,
 * and a weak gravity keeps disconnected components from
 * drifting off.  The layout runs on a ~40Hz timer tick until it
 * settles, and re-warms whenever the session set changes (new
 * nodes start next to their parent, so subtrees visibly bloom
 * out of the spawning node).  Node positions persist across
 * snapshot updates -- a busy-flag change doesn't reshuffle the
 * world.
 *
 * The world is drawn with a perspective projection onto an
 * offscreen buffer (no flicker), nodes depth-sorted painter's
 * style, farther nodes smaller and dimmer.  Idle nodes are cool
 * blue, busy ones bright amber with a pulsing halo.  Drag with
 * button 1 to rotate (with momentum, like radioglobe); scroll
 * to zoom; space toggles a slow ubigraph-style auto-rotation;
 * '0' resets the view.  Hovering a node shows its name, model,
 * busy/idle state, and parent in the status strip at the bottom
 * and labels the node itself.
 *
 * Plumbing (unchanged from the original claudegraph)
 * --------------------------------------------------
 * No polling: "graphlive" is a long-poll file.  The first read
 * on a fresh fid returns the current snapshot right away; every
 * later read on the same fid blocks in the server until the
 * session set changes, then returns the new snapshot.  One
 * watcher proc per source sits in a blocking read loop.  See
 * claude9fs's graphread()/bumpgraph() for the server side.
 * (The sibling "graph" file is the same snapshot but EOF-
 * terminated, for cat(1)/scripts/agent tools.)
 *
 * Each read must supply a buffer big enough for one whole
 * snapshot (Graphbufsz): the protocol has no length framing.
 *
 * A small reconnect fallback (poll every reconnectms) covers a
 * source's claude9fs disappearing or not having started yet.
 *
 * Namespaces: mounting /srv/claude onto /mnt/claude (as
 * claudetalk does) only affects the namespace group that did
 * the mount.  claudegraph is commonly started from some other
 * window, so if a source's mount point does not already work,
 * watchproc calls automount() to mount /srv/<tag> onto it
 * directly.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>

typedef struct Vec3 Vec3;
struct Vec3 {
	double	x, y, z;
};

typedef struct Node Node;
struct Node {
	/* identity, straight from the graph snapshot */
	char	*name;		/* raw session name */
	char	*model;
	int	busy;
	char	*parent;	/* raw parent name, nil if none */
	long	idle;		/* idle seconds as of the snapshot */
	int	srci;		/* index into srcs[] */
	int	parentidx;	/* index into nodes[], -1 if none/unresolved */
	char	*label;		/* display label: "tag:name" or "name" */

	/* force-directed layout state, world coordinates */
	Vec3	pos;
	Vec3	vel;
	Vec3	frc;		/* scratch: force accumulator for one step */
	int	placed;		/* pos is meaningful (scratch during rebuild) */

	/* per-frame projection */
	Point	scr;		/* projected center */
	double	depth;		/* toward-viewer component; larger = nearer */
	int	rad;		/* projected radius, pixels */
};

static char **srcs;		/* mount points to watch */
static int nsrc;
static char **srctag;		/* short display tag per source */
static char **srctext;		/* last snapshot text per source, or nil */
static long *srcsnap;		/* time(0) when srctext[i] was read */

static Node *nodes;
static int nnode;
static int havebusy;		/* any node busy (drives halo animation) */
static int simwarm;		/* layout not yet settled; keep stepping */

/*
 * View state: a camera orbiting the origin.  yaw/pitch in
 * degrees, zoom a plain scale factor.  vyaw/vpitch are the
 * momentum velocities (degrees per millisecond) tracked while
 * dragging and consumed with friction after release, exactly
 * like radioglobe's inertial spin.
 */
static double yaw = -35.0, pitch = 18.0, zoom = 1.0;
static double vyaw, vpitch;
static int autorotate = 1;
static ulong pulse;		/* tick counter for the busy halo */

/* per-frame projection basis, set by setview() */
static Vec3 vex, vey, vez;
static Point vctr;
static double vscale;

/*
 * Index into nodes[] of the node the mouse is over, or -1.
 * Recomputed inside redraw() from mousept, so it can never
 * dangle across a rebuild.  Guarded, like nodes/srctext and the
 * view state, by the display lock: watchprocs and the main proc
 * both take lockdisplay around everything they touch.
 */
static int selnode = -1;
static Point mousept;

/* transient status-strip message (e.g. hangup feedback) */
static char statmsg[256];
static long statmsgexp;

static Image *back;		/* offscreen frame buffer */
static Image *bgcol;
static Image *statbg;

enum {
	Etick	= 4,		/* etimer key; Emouse|Ekeyboard are 1|2 */
	Tickms	= 25,		/* ~40Hz animation */

	Nshade	= 8,		/* brightness levels per palette */
	Hitpad	= 4,		/* hover hit-test slop, pixels */
	Statpad	= 4,
	Boxpad	= 6,

	Graphbufsz = 65536,
};

static Image *idlepal[Nshade];	/* cool blue spheres */
static Image *busypal[Nshade];	/* amber spheres + halo */
static Image *edgepal[Nshade];	/* blue-gray edges */

/*
 * Layout tuning.  World units are abstract; Worldr world units
 * map to half the window's short side at zoom 1.
 */
#define Worldr		3.0	/* view scale */
#define Worldclamp	5.0	/* hard cap on |pos| */
#define Pdist		9.0	/* perspective camera distance */
#define Restlen		1.3	/* edge rest length */
#define Kspring		0.10	/* edge spring constant */
#define Krep		0.50	/* pairwise repulsion */
#define Kcent		0.03	/* gravity toward the origin */
#define Damp		0.80	/* velocity damping per step */
#define Maxv		0.30	/* speed cap per step */
#define Stopv		0.0015	/* below this max speed, layout is settled */
#define Nodesz		10.0	/* node radius in pixels at zoom 1, mid depth */

/* idle fade: full brightness until Fadestart s, Fademin by Fadeend s */
#define Fadestart	120.0
#define Fadeend		900.0
#define Fademin		0.30

/* drag/momentum tuning */
#define Dragdeg		0.35	/* degrees of rotation per pixel dragged */
#define Friction	0.92
#define Vmin		0.0005

static int reconnectms = 2000;

void*
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc: %r");
	memset(p, 0, n);
	return p;
}

void*
erealloc(void *p, ulong n)
{
	p = realloc(p, n);
	if(p == nil && n != 0)
		sysfatal("realloc: %r");
	return p;
}

char*
estrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("strdup: %r");
	return s;
}

static Vec3
vadd(Vec3 a, Vec3 b)
{
	a.x += b.x;
	a.y += b.y;
	a.z += b.z;
	return a;
}

static Vec3
vsub(Vec3 a, Vec3 b)
{
	a.x -= b.x;
	a.y -= b.y;
	a.z -= b.z;
	return a;
}

static Vec3
vmul(Vec3 a, double s)
{
	a.x *= s;
	a.y *= s;
	a.z *= s;
	return a;
}

static double
vdot(Vec3 a, Vec3 b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

/* random vector of length r, for jitter and initial placement */
static Vec3
rndvec(double r)
{
	Vec3 v;
	double m;

	do{
		v.x = frand() - 0.5;
		v.y = frand() - 0.5;
		v.z = frand() - 0.5;
		m = vdot(v, v);
	}while(m < 1e-4);
	return vmul(v, r/sqrt(m));
}

typedef struct Grow Grow;
struct Grow {
	Node	*v;
	int	n;
	int	cap;
};

static void
growadd(Grow *g, char *name, char *model, int busy, char *parent, long idle, int srci)
{
	Node *nd;

	if(g->n >= g->cap){
		g->cap = g->cap ? g->cap*2 : 32;
		g->v = erealloc(g->v, g->cap*sizeof(Node));
	}
	nd = &g->v[g->n++];
	memset(nd, 0, sizeof *nd);
	nd->name = estrdup(name);
	nd->model = estrdup(model);
	nd->busy = busy;
	nd->parent = (parent[0] != '\0' && strcmp(parent, "-") != 0) ?
		estrdup(parent) : nil;
	nd->idle = idle;
	nd->srci = srci;
	nd->parentidx = -1;
}

/*
 * Parse one source's cached "graphlive" text into g, tagged
 * srci.  text is scratch: this modifies it in place (splitting
 * on tabs and newlines), so the caller must not reuse it.
 * The idle field is a later addition to the format; lines from
 * an older claude9fs that lack it parse as idle 0 rather than
 * being dropped.
 */
static void
parsegraph(Grow *g, char *text, int srci)
{
	char *line, *next, *f0, *f1, *f2, *f3, *f4;

	if(text == nil)
		return;
	for(line = text; line != nil && *line != '\0'; line = next){
		next = strchr(line, '\n');
		if(next != nil){
			*next = '\0';
			next++;
		}
		if(*line == '\0')
			continue;
		f0 = line;
		f1 = strchr(f0, '\t');
		if(f1 == nil)
			continue;
		*f1++ = '\0';
		f2 = strchr(f1, '\t');
		if(f2 == nil)
			continue;
		*f2++ = '\0';
		f3 = strchr(f2, '\t');
		if(f3 == nil)
			continue;
		*f3++ = '\0';
		f4 = strchr(f3, '\t');
		if(f4 != nil)
			*f4++ = '\0';
		growadd(g, f0, f1, atoi(f2), f3, f4 != nil ? atol(f4) : 0, srci);
	}
}

static void
freenodes(Node *v, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(v[i].name);
		free(v[i].model);
		free(v[i].parent);
		free(v[i].label);
	}
	free(v);
}

/*
 * Rebuild the combined node array from the cached per-source
 * text (srctext[]).  Called with the display locked.
 *
 * Parent links resolve by name, preferring a session on the
 * same source but falling back to any source: the standard
 * claudetalk arrangement has the orchestrator on /mnt/claude
 * and its sub-agents on /mnt/claudesub, so the edges that
 * matter most are exactly the cross-source ones.
 *
 * Layout positions survive the rebuild: a node that already
 * existed (same source and name) keeps its position and
 * velocity, so snapshot churn (busy flags flipping, siblings
 * coming and going) doesn't reshuffle the picture.  A genuinely
 * new node starts next to its parent (slightly jittered), so
 * spawned sub-agents visibly bud off their spawner; a new root
 * starts at a random spot.  Any change re-warms the simulation.
 */
static void
rebuildnodes(void)
{
	Grow g;
	char *scratch;
	int i, j, pass, changed;
	Node *n, *o;

	memset(&g, 0, sizeof g);
	for(i = 0; i < nsrc; i++){
		if(srctext[i] == nil)
			continue;
		scratch = estrdup(srctext[i]);
		parsegraph(&g, scratch, i);
		free(scratch);
	}

	/* resolve parent name -> index: same source first, then any */
	for(i = 0; i < g.n; i++){
		n = &g.v[i];
		if(n->parent == nil)
			continue;
		for(j = 0; j < g.n; j++)
			if(j != i && g.v[j].srci == n->srci
			&& strcmp(g.v[j].name, n->parent) == 0){
				n->parentidx = j;
				break;
			}
		if(n->parentidx < 0)
			for(j = 0; j < g.n; j++)
				if(j != i && strcmp(g.v[j].name, n->parent) == 0){
					n->parentidx = j;
					break;
				}
	}

	for(i = 0; i < g.n; i++){
		if(nsrc > 1)
			g.v[i].label = smprint("%s:%s", srctag[g.v[i].srci], g.v[i].name);
		else
			g.v[i].label = estrdup(g.v[i].name);
	}

	/* carry positions over from the old array */
	changed = g.n != nnode;
	for(i = 0; i < g.n; i++){
		n = &g.v[i];
		n->placed = 0;
		for(j = 0; j < nnode; j++){
			o = &nodes[j];
			if(o->srci == n->srci && strcmp(o->name, n->name) == 0){
				n->pos = o->pos;
				n->vel = o->vel;
				n->placed = 1;
				/*
				 * A parent label appearing, changing, or
				 * going away means the edge set changed
				 * even though the node set didn't (the
				 * usual case: clone first, tag parent a
				 * moment later).  The spring layout has
				 * to re-warm or the new edge never pulls
				 * the pair together.
				 */
				if((o->parent == nil) != (n->parent == nil)
				|| (o->parent != nil && n->parent != nil
				   && strcmp(o->parent, n->parent) != 0))
					changed = 1;
				break;
			}
		}
		if(!n->placed)
			changed = 1;
	}

	/*
	 * Place new nodes: next to their parent when the parent
	 * already has a position (a few passes so a whole new
	 * chain settles root-first), else at random.
	 */
	for(pass = 0; pass < 4; pass++)
		for(i = 0; i < g.n; i++){
			n = &g.v[i];
			if(n->placed || n->parentidx < 0 || !g.v[n->parentidx].placed)
				continue;
			n->pos = vadd(g.v[n->parentidx].pos, rndvec(0.6));
			n->vel.x = n->vel.y = n->vel.z = 0;
			n->placed = 1;
		}
	for(i = 0; i < g.n; i++){
		n = &g.v[i];
		if(n->placed)
			continue;
		n->pos = rndvec(1.5 + frand());
		n->vel.x = n->vel.y = n->vel.z = 0;
		n->placed = 1;
	}

	freenodes(nodes, nnode);
	nodes = g.v;
	nnode = g.n;

	havebusy = 0;
	for(i = 0; i < nnode; i++)
		if(nodes[i].busy)
			havebusy = 1;
	if(changed)
		simwarm = 1;
}

/*
 * One step of the spring embedder.  Returns the largest node
 * speed this step, so the caller can tell when the layout has
 * settled and stop burning ticks on it.  O(n^2), which is
 * nothing at session-graph scale.
 */
static double
physstep(void)
{
	int i, j;
	Node *a, *b;
	Vec3 d;
	double d2, dist, mag, maxv, v;

	if(nnode == 0)
		return 0.0;

	for(i = 0; i < nnode; i++)
		nodes[i].frc = vmul(nodes[i].pos, -Kcent);

	/* pairwise repulsion */
	for(i = 0; i < nnode; i++)
		for(j = i+1; j < nnode; j++){
			a = &nodes[i];
			b = &nodes[j];
			d = vsub(a->pos, b->pos);
			d2 = vdot(d, d);
			if(d2 < 1e-4){
				/* coincident: shove apart in a random direction */
				d = rndvec(0.05);
				d2 = vdot(d, d);
			}
			dist = sqrt(d2);
			mag = Krep / d2;
			a->frc = vadd(a->frc, vmul(d, mag/dist));
			b->frc = vsub(b->frc, vmul(d, mag/dist));
		}

	/* springs along parent-child edges */
	for(i = 0; i < nnode; i++){
		a = &nodes[i];
		if(a->parentidx < 0)
			continue;
		b = &nodes[a->parentidx];
		d = vsub(b->pos, a->pos);
		dist = sqrt(vdot(d, d));
		if(dist < 1e-3)
			continue;
		mag = Kspring * (dist - Restlen);
		a->frc = vadd(a->frc, vmul(d, mag/dist));
		b->frc = vsub(b->frc, vmul(d, mag/dist));
	}

	/* integrate with damping, a speed cap, and a world cap */
	maxv = 0.0;
	for(i = 0; i < nnode; i++){
		a = &nodes[i];
		a->vel = vmul(vadd(a->vel, a->frc), Damp);
		v = sqrt(vdot(a->vel, a->vel));
		if(v > Maxv){
			a->vel = vmul(a->vel, Maxv/v);
			v = Maxv;
		}
		a->pos = vadd(a->pos, a->vel);
		d2 = vdot(a->pos, a->pos);
		if(d2 > Worldclamp*Worldclamp)
			a->pos = vmul(a->pos, Worldclamp/sqrt(d2));
		if(v > maxv)
			maxv = v;
	}
	return maxv;
}

/* height of the bottom status strip */
static int
statheight(void)
{
	return font->height + 2*Statpad;
}

/* the 3D viewport: the whole window minus the status strip */
static Rectangle
viewrect(void)
{
	Rectangle r;

	r = screen->r;
	r.max.y -= statheight();
	return r;
}

/*
 * Compute the projection basis for the current yaw/pitch and
 * the scale/center for rectangle r.  vex/vey are the screen-
 * right/screen-up unit vectors, vez points at the viewer --
 * the same scheme as radioglobe's viewbasis(), with yaw/pitch
 * playing lon/lat.
 */
static void
setview(Rectangle r)
{
	double cla, sla, clo, slo, m;

	cla = cos(pitch * PI/180.0);
	sla = sin(pitch * PI/180.0);
	clo = cos(yaw * PI/180.0);
	slo = sin(yaw * PI/180.0);

	vex.x = -slo;      vex.y = clo;       vex.z = 0;
	vey.x = -sla*clo;  vey.y = -sla*slo;  vey.z = cla;
	vez.x = cla*clo;   vez.y = cla*slo;   vez.z = sla;

	vctr = Pt((r.min.x + r.max.x)/2, (r.min.y + r.max.y)/2);
	m = Dx(r) < Dy(r) ? Dx(r) : Dy(r);
	vscale = zoom * (m/2.0) * 0.85 / Worldr;
}

static void
projectnode(Node *n)
{
	double px, py, pz, f;

	px = vdot(n->pos, vex);
	py = vdot(n->pos, vey);
	pz = vdot(n->pos, vez);

	/* perspective: nearer (larger pz) is bigger */
	f = Pdist / (Pdist - pz);
	if(f < 0.1)
		f = 0.1;

	n->scr.x = vctr.x + (int)(px * vscale * f);
	n->scr.y = vctr.y - (int)(py * vscale * f);
	n->depth = pz;
	n->rad = (int)(Nodesz * f * zoom + 0.5);
	if(n->rad < 3)
		n->rad = 3;
	if(n->rad > 60)
		n->rad = 60;
}

/*
 * Hit-test a point against the projected node circles; returns
 * the index of the frontmost node under xy, or -1.  Called with
 * the display locked, against the projection from the last
 * redraw.
 */
static int
nodeat(Point xy)
{
	int i, dx, dy, rad, best;
	double bestz;

	best = -1;
	bestz = -1e9;
	for(i = 0; i < nnode; i++){
		rad = nodes[i].rad + Hitpad;
		dx = xy.x - nodes[i].scr.x;
		dy = xy.y - nodes[i].scr.y;
		if(dx*dx + dy*dy <= rad*rad && nodes[i].depth > bestz){
			best = i;
			bestz = nodes[i].depth;
		}
	}
	return best;
}

/*
 * A node's effective idle age right now: the server-reported
 * value plus however long ago we read the snapshot it came
 * from.  (The server only reports a point-in-time number; it
 * would be silly to bump the long-poll every second just to
 * age it.)
 */
static long
effidle(Node *n, long now)
{
	long t;

	if(n->busy)
		return 0;
	t = n->idle + (now - srcsnap[n->srci]);
	if(t < 0)
		t = 0;
	return t;
}

/* brightness from depth: nearer is brighter */
static double
depthshade(double depth)
{
	double t;

	t = (depth + Worldr) / (2.0*Worldr);
	if(t < 0.0)
		t = 0.0;
	if(t > 1.0)
		t = 1.0;
	return 0.40 + 0.60*t;
}

/*
 * Brightness from idle age: a session nobody has talked to in
 * a while fades toward the background, so an orphaned
 * sub-agent looks like the ghost it is instead of sitting in
 * the graph at full brightness forever.
 */
static double
idlefade(long idle)
{
	double t;

	if(idle <= Fadestart)
		return 1.0;
	if(idle >= Fadeend)
		return Fademin;
	t = (idle - Fadestart) / (Fadeend - Fadestart);
	return 1.0 - t*(1.0 - Fademin);
}

static int
shadeidx(double s)
{
	int i;

	i = (int)(s * Nshade);
	if(i < 0)
		i = 0;
	if(i >= Nshade)
		i = Nshade - 1;
	return i;
}

static char*
idlefmt(long s, char *buf, int nbuf)
{
	if(s < 60)
		snprint(buf, nbuf, "%lds", s);
	else if(s < 3600)
		snprint(buf, nbuf, "%ldm%02lds", s/60, s%60);
	else
		snprint(buf, nbuf, "%ldh%02ldm", s/3600, (s%3600)/60);
	return buf;
}

static void
setmsg(char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsnprint(statmsg, sizeof statmsg, fmt, arg);
	va_end(arg);
	statmsgexp = time(0) + 4;
}

/*
 * Draw the bottom status strip onto back.  With a node hovered
 * it shows that session's full detail; otherwise a summary and
 * the control cheatsheet.  A transient message (hangup
 * feedback) overrides both for a few seconds.
 */
static void
drawstatus(long now)
{
	Rectangle r;
	Point p;
	Node *n;
	char buf[512], ib[32];
	int i, nbusy;

	r = screen->r;
	r.min.y = r.max.y - statheight();
	draw(back, r, statbg, nil, ZP);
	p = addpt(r.min, Pt(Boxpad, Statpad));

	if(statmsg[0] != '\0' && now < statmsgexp)
		snprint(buf, sizeof buf, "%s", statmsg);
	else if(selnode >= 0 && selnode < nnode){
		n = &nodes[selnode];
		if(n->busy)
			snprint(buf, sizeof buf, "%s   model %s   BUSY   parent %s",
				n->label, n->model,
				n->parent != nil ? n->parent : "-");
		else
			snprint(buf, sizeof buf, "%s   model %s   idle %s   parent %s",
				n->label, n->model,
				idlefmt(effidle(n, now), ib, sizeof ib),
				n->parent != nil ? n->parent : "-");
	}else{
		nbusy = 0;
		for(i = 0; i < nnode; i++)
			if(nodes[i].busy)
				nbusy++;
		snprint(buf, sizeof buf,
			"%d session%s, %d busy, %d source%s   "
			"drag rotate - scroll zoom - space spin - 0 reset - b3 hangup - q quit",
			nnode, nnode == 1 ? "" : "s",
			nbusy,
			nsrc, nsrc == 1 ? "" : "s");
	}
	string(back, p, display->white, ZP, font, buf);
}

/*
 * Draw one node as a fake-shaded sphere: body fill at the
 * depth/idle-derived brightness, a darker rim, and a small
 * off-center highlight.  Busy nodes get a pulsing halo ring.
 */
static void
drawnode(Node *n, int hover, long now)
{
	Image **pal;
	int idx, hi, lo, r;
	double shade;

	shade = depthshade(n->depth) * idlefade(effidle(n, now));
	idx = shadeidx(shade);
	pal = n->busy ? busypal : idlepal;

	if(n->busy){
		r = n->rad + 3 + (int)(2.5 + 2.5*sin(pulse * 0.25));
		ellipse(back, n->scr, r, r, 0,
			busypal[idx >= 3 ? idx-3 : 0], ZP);
	}

	fillellipse(back, n->scr, n->rad, n->rad, pal[idx], ZP);
	lo = idx >= 3 ? idx-3 : 0;
	ellipse(back, n->scr, n->rad, n->rad, 0, pal[lo], ZP);
	hi = idx+2 >= Nshade ? Nshade-1 : idx+2;
	fillellipse(back, addpt(n->scr, Pt(-n->rad/3, -n->rad/3)),
		n->rad/4 + 1, n->rad/4 + 1, pal[hi], ZP);

	if(hover)
		ellipse(back, n->scr, n->rad+2, n->rad+2, 1, display->white, ZP);
}

/*
 * Render a frame: project everything, depth-sort, draw edges
 * then nodes painter's style onto the offscreen buffer, then
 * blit it in one go.  Called with the display locked.
 */
static void
redraw(void)
{
	int i, j, k, idx, thick, *order;
	static int *orderbuf;
	static int ordercap;
	long now;
	Node *n, *pn;
	Rectangle vr;
	char *s;
	Point p0, p1;

	if(back == nil || !eqrect(back->r, screen->r)){
		freeimage(back);
		back = allocimage(display, screen->r, screen->chan, 0, DNofill);
		if(back == nil)
			sysfatal("allocimage: %r");
	}

	now = time(0);
	vr = viewrect();
	draw(back, back->r, bgcol, nil, ZP);

	if(nnode == 0){
		selnode = -1;
		string(back, addpt(vr.min, Pt(20, 20)),
			edgepal[Nshade-1], ZP, font, "claudegraph: no sessions");
		drawstatus(now);
		draw(screen, screen->r, back, nil, screen->r.min);
		flushimage(display, 1);
		return;
	}

	setview(vr);
	for(i = 0; i < nnode; i++)
		projectnode(&nodes[i]);

	/* hover follows the last known mouse position */
	selnode = nodeat(mousept);

	/*
	 * Edges first, so the spheres draw over the line ends.
	 * Edge brightness follows the average depth of its
	 * endpoints; edges touching the hovered node draw bright
	 * and thick so its connectivity pops out.
	 */
	for(i = 0; i < nnode; i++){
		n = &nodes[i];
		if(n->parentidx < 0)
			continue;
		pn = &nodes[n->parentidx];
		if(i == selnode || n->parentidx == selnode){
			idx = Nshade - 1;
			thick = 1;
		}else{
			idx = shadeidx(depthshade((n->depth + pn->depth)/2.0));
			thick = 0;
		}
		p0 = pn->scr;
		p1 = n->scr;
		line(back, p0, p1, Enddisc, Enddisc, thick, edgepal[idx], ZP);
	}

	/* depth sort, far to near (insertion sort; n is tiny) */
	if(ordercap < nnode){
		ordercap = nnode + 32;
		orderbuf = erealloc(orderbuf, ordercap*sizeof(int));
	}
	order = orderbuf;
	for(i = 0; i < nnode; i++)
		order[i] = i;
	for(i = 1; i < nnode; i++){
		k = order[i];
		for(j = i-1; j >= 0 && nodes[order[j]].depth > nodes[k].depth; j--)
			order[j+1] = order[j];
		order[j+1] = k;
	}

	for(i = 0; i < nnode; i++){
		k = order[i];
		drawnode(&nodes[k], k == selnode, now);
	}

	/* label the hovered node in place: name and model */
	if(selnode >= 0){
		n = &nodes[selnode];
		s = smprint("%s (%s)", n->label, n->model);
		string(back, Pt(n->scr.x + n->rad + 6, n->scr.y - font->height/2),
			display->white, ZP, font, s);
		free(s);
	}

	drawstatus(now);
	draw(screen, screen->r, back, nil, screen->r.min);
	flushimage(display, 1);
}

void
eresized(int new)
{
	lockdisplay(display);
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("can't reattach to window: %r");
	redraw();
	unlockdisplay(display);
}

/*
 * Best-effort self-mount fallback.
 *
 * claudegraph is often started from a different rio window (and
 * hence a different namespace group, per bind(2)) than the one
 * that ran claudetalk and did "mount -c /srv/claude /mnt/claude".
 * That mount is private to the process group that made it, so a
 * plain open() of mtpt/graphlive from anywhere else just fails.
 *
 * /srv, though, is visible from any namespace descended from the
 * boot environment, so if opening mtpt/graphlive fails we mount
 * /srv/tag onto mtpt ourselves and try again.  tag is the last
 * path element of mtpt (e.g. "claude" for "/mnt/claude"), which
 * is the convention claudetalk uses for its srv names.  If that
 * guess is wrong, or the srv file plain doesn't exist, this
 * quietly does nothing and the caller falls back to its normal
 * reconnect/poll behavior.
 */
static void
automount(char *mtpt, char *tag)
{
	int fd;
	char *srvpath;

	/* idempotent: fine if mtpt already exists as a directory */
	fd = create(mtpt, OREAD, DMDIR|0777);
	if(fd >= 0)
		close(fd);

	srvpath = smprint("/srv/%s", tag);
	fd = open(srvpath, ORDWR);
	free(srvpath);
	if(fd < 0)
		return;
	if(mount(fd, -1, mtpt, MREPL, "") < 0)
		close(fd);
}

/*
 * One of these runs per source, forked with RFPROC|RFMEM (so it
 * shares nodes/srctext/the display with the main proc, Plan 9
 * "thread" style).  Steady state is a blocking read loop with
 * no sleep at all: each read either returns straight away
 * (first read on the fid) or blocks in claude9fs until the
 * graph actually changes.  Reconnection after an error is the
 * only place this proc sleeps/polls.
 */
static void
watchproc(void *arg)
{
	int srci, fd, n;
	char *path, *buf;

	srci = (int)(uintptr)arg;
	path = smprint("%s/graphlive", srcs[srci]);

	for(;;){
		fd = open(path, OREAD);
		if(fd < 0){
			/* maybe we just need to mount it ourselves */
			automount(srcs[srci], srctag[srci]);
			fd = open(path, OREAD);
		}
		if(fd < 0){
			lockdisplay(display);
			free(srctext[srci]);
			srctext[srci] = nil;
			rebuildnodes();
			redraw();
			unlockdisplay(display);
			sleep(reconnectms);
			continue;
		}

		for(;;){
			buf = emalloc(Graphbufsz);
			n = read(fd, buf, Graphbufsz-1);
			if(n <= 0){
				free(buf);
				break;	/* source gone (or server restarted); reopen */
			}
			buf[n] = '\0';
			lockdisplay(display);
			free(srctext[srci]);
			srctext[srci] = buf;
			srcsnap[srci] = time(0);
			rebuildnodes();
			redraw();
			unlockdisplay(display);
		}

		close(fd);
		lockdisplay(display);
		free(srctext[srci]);
		srctext[srci] = nil;
		rebuildnodes();
		redraw();
		unlockdisplay(display);
		sleep(reconnectms);
	}
}

static void
mkpalettes(void)
{
	int i;
	ulong v;
	double f;
	/* base colors: idle cool blue, busy amber, edges blue-gray */
	int ir = 0x50, ig = 0xB8, ib = 0xFF;
	int br = 0xFF, bg = 0xB8, bb = 0x28;
	int er = 0x78, eg = 0x90, eb = 0xB0;

	for(i = 0; i < Nshade; i++){
		f = (double)(i+1) / Nshade;
		v = ((ulong)(ir*f)<<24) | ((ulong)(ig*f)<<16) | ((ulong)(ib*f)<<8) | 0xFF;
		idlepal[i] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, v);
		v = ((ulong)(br*f)<<24) | ((ulong)(bg*f)<<16) | ((ulong)(bb*f)<<8) | 0xFF;
		busypal[i] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, v);
		v = ((ulong)(er*f)<<24) | ((ulong)(eg*f)<<16) | ((ulong)(eb*f)<<8) | 0xFF;
		edgepal[i] = allocimage(display, Rect(0,0,1,1), screen->chan, 1, v);
		if(idlepal[i] == nil || busypal[i] == nil || edgepal[i] == nil)
			sysfatal("allocimage: %r");
	}
	bgcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x000014FF);
	statbg = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x141414FF);
	if(bgcol == nil)
		bgcol = display->black;
	if(statbg == nil)
		statbg = display->black;
}

static char *nodemenuitems[] = {
	"hangup",
	nil,
};
static Menu nodemenu = { nodemenuitems };

static void
usage(void)
{
	fprint(2, "usage: claudegraph [-T reconnectms] [mtpt ...]\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Event e;
	Mouse m;
	int i, anim, dragging, oldbuttons, hsrc, fd;
	Point dragstart;
	double dragyaw, dragpitch, lastyaw, lastpitch, dt, dy;
	ulong lastmsec;
	char *hname, *hlabel, *path;

	ARGBEGIN{
	case 'T':
		reconnectms = atoi(EARGF(usage()));
		if(reconnectms <= 0)
			reconnectms = 2000;
		break;
	default:
		usage();
	}ARGEND

	if(argc == 0){
		static char *def[] = { "/mnt/claude", "/mnt/claudesub" };
		srcs = def;
		nsrc = nelem(def);
	}else{
		srcs = argv;
		nsrc = argc;
	}
	srctag = emalloc(nsrc*sizeof(char*));
	srctext = emalloc(nsrc*sizeof(char*));
	srcsnap = emalloc(nsrc*sizeof(long));
	for(i = 0; i < nsrc; i++){
		char *p;
		p = strrchr(srcs[i], '/');
		srctag[i] = p != nil ? p+1 : srcs[i];
	}

	srand(time(0) ^ (getpid()<<8));

	if(initdraw(nil, nil, "claudegraph") < 0)
		sysfatal("initdraw: %r");
	mkpalettes();
	einit(Ekeyboard|Emouse);
	if(etimer(Etick, Tickms) != Etick)
		fprint(2, "claudegraph: etimer failed; no animation\n");

	redraw();	/* "no sessions" placeholder until the watchers report in */
	unlockdisplay(display);

	for(i = 0; i < nsrc; i++){
		switch(rfork(RFPROC|RFMEM)){
		case -1:
			sysfatal("rfork: %r");
		case 0:
			watchproc((void*)(uintptr)i);
			exits(nil);
		}
	}

	dragging = 0;
	oldbuttons = 0;
	dragstart = ZP;
	dragyaw = yaw;
	dragpitch = pitch;
	lastyaw = yaw;
	lastpitch = pitch;
	lastmsec = 0;

	for(;;)
		switch(event(&e)){
		case Ekeyboard:
			switch(e.kbdc){
			case Kdel:
			case 'q':
				exits(nil);
			case ' ':
				lockdisplay(display);
				autorotate = !autorotate;
				unlockdisplay(display);
				break;
			case '0':
				lockdisplay(display);
				yaw = -35.0;
				pitch = 18.0;
				zoom = 1.0;
				vyaw = vpitch = 0.0;
				redraw();
				unlockdisplay(display);
				break;
			case '+':
			case '=':
				lockdisplay(display);
				zoom *= 1.3;
				if(zoom > 8.0) zoom = 8.0;
				redraw();
				unlockdisplay(display);
				break;
			case '-':
				lockdisplay(display);
				zoom /= 1.3;
				if(zoom < 0.2) zoom = 0.2;
				redraw();
				unlockdisplay(display);
				break;
			case Kleft:
				lockdisplay(display);
				yaw -= 10.0;
				redraw();
				unlockdisplay(display);
				break;
			case Kright:
				lockdisplay(display);
				yaw += 10.0;
				redraw();
				unlockdisplay(display);
				break;
			case Kup:
				lockdisplay(display);
				pitch += 10.0;
				if(pitch > 85.0) pitch = 85.0;
				redraw();
				unlockdisplay(display);
				break;
			case Kdown:
				lockdisplay(display);
				pitch -= 10.0;
				if(pitch < -85.0) pitch = -85.0;
				redraw();
				unlockdisplay(display);
				break;
			}
			break;

		case Emouse:
			m = e.mouse;

			/* scroll wheel zoom */
			if(m.buttons & 8){
				lockdisplay(display);
				zoom *= 1.15;
				if(zoom > 8.0) zoom = 8.0;
				redraw();
				unlockdisplay(display);
				oldbuttons = m.buttons;
				break;
			}
			if(m.buttons & 16){
				lockdisplay(display);
				zoom /= 1.15;
				if(zoom < 0.2) zoom = 0.2;
				redraw();
				unlockdisplay(display);
				oldbuttons = m.buttons;
				break;
			}

			/* button 1: drag to rotate, with momentum */
			if(m.buttons & 1){
				lockdisplay(display);
				if(!dragging && !(oldbuttons & 1)){
					dragging = 1;
					dragstart = m.xy;
					dragyaw = yaw;
					dragpitch = pitch;
					/* grabbing a spinning graph stops it dead */
					vyaw = vpitch = 0.0;
					lastyaw = yaw;
					lastpitch = pitch;
					lastmsec = m.msec;
				}
				if(dragging){
					yaw = dragyaw - (double)(m.xy.x - dragstart.x)*Dragdeg;
					pitch = dragpitch + (double)(m.xy.y - dragstart.y)*Dragdeg;
					if(pitch > 85.0) pitch = 85.0;
					if(pitch < -85.0) pitch = -85.0;
					while(yaw > 180.0) yaw -= 360.0;
					while(yaw < -180.0) yaw += 360.0;
					if(m.msec > lastmsec){
						dt = m.msec - lastmsec;
						dy = yaw - lastyaw;
						while(dy > 180.0) dy -= 360.0;
						while(dy < -180.0) dy += 360.0;
						vyaw = dy / dt;
						vpitch = (pitch - lastpitch) / dt;
						lastyaw = yaw;
						lastpitch = pitch;
						lastmsec = m.msec;
					}
					redraw();
				}
				unlockdisplay(display);
			}else
				dragging = 0;

			/*
			 * button 3 over a node: hangup menu.  Copy the
			 * node's identity out before dropping the lock;
			 * the node array can be rebuilt while the menu
			 * is up.
			 */
			if((m.buttons & 4) && !(oldbuttons & 4)){
				hname = nil;
				hlabel = nil;
				hsrc = 0;
				lockdisplay(display);
				i = nodeat(m.xy);
				if(i >= 0){
					hname = estrdup(nodes[i].name);
					hlabel = estrdup(nodes[i].label);
					hsrc = nodes[i].srci;
				}
				unlockdisplay(display);
				if(hname != nil && emenuhit(3, &m, &nodemenu) == 0){
					path = smprint("%s/%s/ctl", srcs[hsrc], hname);
					fd = open(path, OWRITE);
					lockdisplay(display);
					if(fd < 0)
						setmsg("%s: can't open ctl: %r", hlabel);
					else{
						if(write(fd, "hangup", 6) < 0)
							setmsg("%s: hangup failed: %r", hlabel);
						else
							setmsg("hung up %s", hlabel);
						close(fd);
					}
					redraw();
					unlockdisplay(display);
					free(path);
				}
				free(hname);
				free(hlabel);
			}

			/* hover */
			if(m.buttons == 0 && !dragging){
				lockdisplay(display);
				mousept = m.xy;
				i = nodeat(mousept);
				if(i != selnode)
					redraw();
				unlockdisplay(display);
			}

			oldbuttons = m.buttons;
			break;

		case Etick:
			/*
			 * Don't let animation starve input: if a
			 * keystroke is already waiting, skip this
			 * tick's work (same trick as radioglobe).
			 */
			if(ecankbd())
				break;
			lockdisplay(display);
			anim = 0;
			if(!dragging && (vyaw != 0.0 || vpitch != 0.0)){
				yaw += vyaw * Tickms;
				pitch += vpitch * Tickms;
				if(pitch > 85.0){ pitch = 85.0; vpitch = 0.0; }
				if(pitch < -85.0){ pitch = -85.0; vpitch = 0.0; }
				while(yaw > 180.0) yaw -= 360.0;
				while(yaw < -180.0) yaw += 360.0;
				vyaw *= Friction;
				vpitch *= Friction;
				if(fabs(vyaw) < Vmin && fabs(vpitch) < Vmin)
					vyaw = vpitch = 0.0;
				anim = 1;
			}
			if(autorotate && !dragging && nnode > 0){
				yaw += 0.12;
				if(yaw > 180.0)
					yaw -= 360.0;
				anim = 1;
			}
			if(simwarm){
				if(physstep() < Stopv)
					simwarm = 0;
				anim = 1;
			}
			if(havebusy){
				pulse++;
				anim = 1;
			}
			if(anim)
				redraw();
			unlockdisplay(display);
			break;
		}
}
