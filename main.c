/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * snoopy - network sniffer
 */
#define _GNU_SOURCE 1
#include "ip.h"
#include <stdio.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <parlib/printf-ext.h>
#include <ndblib/fcallfmt.h>

//#include <fcall.h>
//#include <libsec.h>
//#include <ndb.h>
#include "dat.h"
#include "protos.h"
#include "y.tab.h"

int Cflag;
int Nflag;
int Mflag;
int sflag;
int tiflag;
int toflag;
char *argv0;
char *buf;

char *prom = "promiscuous";
char *oneblock = "oneblock";

enum
{
	Pktlen=	64*1024,
	Blen=	16*1024,
	Pcaphdrlen = 16,
	Fakeethhdrlen = 14,
};

Filter *filter;
Proto *root;
int64_t starttime, pkttime;
int pcap;

int	filterpkt(Filter *f, uint8_t *ps, uint8_t *pe, Proto *pr, int);
void	printpkt(char *p, char *e, uint8_t *ps, uint8_t *pe);
void	mkprotograph(void);
Proto*	findproto(char *name);
Filter*	compile(Filter *f);
void	printfilter(Filter *f, char *tag);
void	printhelp(char*);
void	tracepkt(uint8_t*, int);
void	pcaphdr(void);

void
printusage(void)
{
	fprintf(stderr, "usage: %s [-CDdpst] [-N n] [-f filter] [-h first-header] path\n", argv0);
	fprintf(stderr, "  for protocol help: %s -? [proto]\n", argv0);
}

void
usage(void)
{
	printusage();
	exit(1);
}

static struct option long_options[] = {
	{"help",          no_argument,       0, '?'},
	{"C",         no_argument,       0, 'C'},
	{"d",         no_argument,       0, 'd'},
	{"D",         no_argument,       0, 'D'},
	{"p",         no_argument,       0, 'p'},
	{"t",         no_argument,       0, 't'},
	{"s",          no_argument,       0, 's'},

	{"proto",          required_argument,       0, 'h'},
	{"M",          required_argument,       0, 'M'},
	{"N",          required_argument,       0, 'N'},
	{"f",          required_argument,       0, 'f'},
	{}
};

/* Akaros's printf function for IP addrs expects the v6 struct that can also
 * hold a v4.  Snoopy seems to have a different header in ip.c, where IPv4 addrs
 * are just 4 bytes. */
static int __printf_ipv4addr(FILE *stream, uint8_t *ipaddr)
{
	return fprintf(stream, "%d.%d.%d.%d", ipaddr[0], ipaddr[1],
	               ipaddr[2], ipaddr[3]);
}

static int printf_ipv4addr(FILE *stream, const struct printf_info *info,
                           const void *const *args)
{
    /* args is an array of pointers, each of which points to an arg.
     * to extract: TYPE x = *(TYPE*)args[n]. */
    uint8_t *ipaddr = *(uint8_t**)args[0];
    return __printf_ipv4addr(stream, ipaddr);
}

static int printf_ipv4addr_info(const struct printf_info* info, size_t n, int *argtypes,
                                int *size)
{
    /* seems like this is how many va_args we will use, and how big each was
     * we're supposed to fill up to n, i think.  we're only doing one */
    if (n > 0) {
        argtypes[0] = PA_POINTER;
        size[0] = sizeof(uint8_t*);
    }
    /* returns the nr of args required by the format string, no matter what */
    return 1;
}

/* We just need any ether, not necessarily ether0.  ifconfig currently attaches
 * just one NIC. */
static const char *get_first_ether(void)
{
	const char *ret = "/net/ether0";
	DIR *net;
	struct dirent *dirent;

	net = opendir("/net");
	if (!net)
		return ret;
	while ((dirent = readdir(net))) {
		/* ether\0 is 6 chars - we match on the first 5 */
		if (!strncmp("ether", dirent->d_name, 5)) {
			ret = strdup(dirent->d_name);
			break;
		}
	}
	closedir(net);
	return ret;
}

int
main(int argc, char **argv)
{
	int option_index;
	uint8_t *pkt;
	char *buf, *p, *e;
	const char *file;
	int fd, cfd;
	int n;
	char c;

	argv0 = argv[0];

	if (register_printf_specifier('E', printf_ethaddr, printf_ethaddr_info))
		printf("Failed to register 'E'\n");
	/* Might have trouble with %I.  Not sure on the details. */
	if (register_printf_specifier('I', printf_ipaddr, printf_ipaddr_info))
		printf("Failed to register 'I'\n");
	/* I think they wanted %V to be IPv4 */
	if (register_printf_specifier('V', printf_ipv4addr, printf_ipv4addr_info))
		printf("Failed to register 'V'\n");
	if (register_printf_specifier('F', printf_fcall, printf_fcall_info))
		printf("Failed to register 'F'\n");
	if (register_printf_specifier('H', printf_hexdump, printf_hexdump_info))
		printf("Failed to register 'H'\n");

	pkt = malloc(Pktlen + Pcaphdrlen + Fakeethhdrlen);
	pkt += Pcaphdrlen + Fakeethhdrlen;
	buf = malloc(Blen);
	e = buf+Blen-1;

	Nflag = 32;
	sflag = 0;

	mkprotograph();

	while ((c = getopt_long(argc, argv, "?CdDtsh:M:N:f:", long_options,
	                        &option_index)) != -1) {
		switch (c) {
		case '?':
		default:
		printusage();
		//printhelp(ARGF());
		exit(0);
		break;
	case 'M':
		p = optarg;
		Mflag = atoi(p);
		break;
	case 'N':
		p = optarg;
		Nflag = atoi(p);
		break;
	case 'f':
		p = optarg;
		yyinit(p);
		yyparse();
		break;
	case 's':
		sflag = 1;
		break;
	case 'h':
		p = optarg;
		root = findproto(p);
		if(root == NULL)
			sysfatal("unknown protocol: %s", p);
		break;
	case 'd':
		toflag = 1;
		break;
	case 'D':
		toflag = 1;
		pcap = 1;
		break;
	case 't':
		tiflag = 1;
		break;
	case 'C':
		Cflag = 1;
		break;
		}
	}

	if(pcap)
		pcaphdr();

	/* next un-processed arg is the [packet-source] */
	if(argc == optind){
		file = get_first_ether();
		if(root != NULL)
			root = &ether;
	} else
		file = argv[optind];

	if((!tiflag) && strstr(file, "ether")){
		if(root == NULL)
			root = &ether;
		snprintf(buf, Blen, "%s!-1", file);
		fd = dial9(buf, 0, 0, &cfd, 0);
		if(fd < 0)
			sysfatal("Error dialing %s: %r", buf);
		if(write(cfd, oneblock, strlen(oneblock)) < 0)
			sysfatal("Error setting %s: %r", oneblock);
		if(write(cfd, prom, strlen(prom)) < 0)
			sysfatal("Error setting %s: %r", prom);
	} else if((!tiflag) && strstr(file, "ipifc")){
		if(root == NULL)
			root = &ip;
		snprintf(buf, Blen, "%s/snoop", file);
		fd = open(buf, O_RDONLY);
		if(fd < 0)
			sysfatal("Error opening %s: %r", buf);
	} else {
		if(root == NULL)
			root = &ether;
		fd = open(file, O_RDONLY);
		if(fd < 0)
			sysfatal("Error opening %s: %r", file);
	}
	filter = compile(filter);

	if(tiflag){
		/* read a trace file */
		for(;;){
			n = read(fd, pkt, 10);
			if(n != 10)
				break;
			pkttime = NetL(pkt+2);
			pkttime = (pkttime<<32) | NetL(pkt+6);
			if(starttime == 0LL)
				starttime = pkttime;
			n = NetS(pkt);
			if(readn(fd, pkt, n) != n)
				break;
			if(filterpkt(filter, pkt, pkt+n, root, 1)){
				if(toflag)
					tracepkt(pkt, n);
				else
					printpkt(buf, e, pkt, pkt+n);
			}
		}
	} else {
		/* read a real time stream */
		starttime = epoch_nsec();
		for(;;){
			n = root->framer(fd, pkt, Pktlen);
			if(n <= 0)
				break;
			pkttime = epoch_nsec();
			if(filterpkt(filter, pkt, pkt+n, root, 1)){
				if(toflag)
					tracepkt(pkt, n);
				else
					printpkt(buf, e, pkt, pkt+n);
			}
		}
	}
}

/* create a new filter node */
Filter*
newfilter(void)
{
	Filter *f;

	f = calloc(1, sizeof(*f));
	if(f == NULL)
		sysfatal( "newfilter: %r");
	return f;
}

/*
 *  apply filter to packet
 */
int
_filterpkt(Filter *f, Msg *m)
{
	Msg ma;

	if(f == NULL)
		return 1;

	switch(f->op){
	case '!':
		return !_filterpkt(f->l, m);
	case LAND:
		ma = *m;
		return _filterpkt(f->l, &ma) && _filterpkt(f->r, m);
	case LOR:
		ma = *m;
		return _filterpkt(f->l, &ma) || _filterpkt(f->r, m);
	case WORD:
		if(m->needroot){
			if(m->pr != f->pr)
				return 0;
			m->needroot = 0;
		}else{
			if(m->pr && (m->pr->filter==NULL || !(m->pr->filter)(f, m)))
				return 0;
		}
		if(f->l == NULL)
			return 1;
		m->pr = f->pr;
		return _filterpkt(f->l, m);
	}
	sysfatal( "internal error: filterpkt op: %d", f->op);
	return 0;
}
int
filterpkt(Filter *f, uint8_t *ps, uint8_t *pe, Proto *pr, int needroot)
{
	Msg m;

	if(f == NULL)
		return 1;

	m.needroot = needroot;
	m.ps = ps;
	m.pe = pe;
	m.pr = pr;
	return _filterpkt(f, &m);
}

/*
 *  from the Unix world
 */
#define PCAP_VERSION_MAJOR 2
#define PCAP_VERSION_MINOR 4
#define TCPDUMP_MAGIC 0xa1b23c4d

struct pcap_file_header {
	uint32_t		magic;
	uint16_t		version_major;
	uint16_t		version_minor;
	int32_t		thiszone;    /* gmt to local correction */
	uint32_t		sigfigs;    /* accuracy of timestamps */
	uint32_t		snaplen;    /* max length saved portion of each pkt */
	uint32_t		linktype;   /* data link type (DLT_*) */
};

struct pcap_pkthdr {
        uint32_t	ts_sec;	/* time stamp */
        uint32_t	ts_nsec;
        uint32_t	caplen;	/* length of portion present */
        uint32_t	len;	/* length this packet (off wire) */
};

/*
 *  pcap trace header 
 */
void
pcaphdr(void)
{
	struct pcap_file_header hdr;

	hdr.magic = TCPDUMP_MAGIC;
	hdr.version_major = PCAP_VERSION_MAJOR;
	hdr.version_minor = PCAP_VERSION_MINOR;
  
	hdr.thiszone = 0;
	hdr.snaplen = Mflag ? Mflag : UINT_MAX;
	hdr.sigfigs = 0;
	hdr.linktype = 1;

	write(1, &hdr, sizeof(hdr));
}

/* This is a bit hacky, assuming the world is ethernet. */
static bool proto_is_link_layer(Proto *p)
{
	return p == &ether;
}

/* This also fakes an IP packet, in lieu of reading the packet. */
static char fake_ethernet_header[] = {
	0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x05, 0x06,
	0x08, 0x00,
};

/*
 *  write out a packet trace
 */
void
tracepkt(uint8_t *ps, int len)
{
	struct pcap_pkthdr *goo;
	size_t hdrlen = Pcaphdrlen;
	char *fake_eth;

	if(Mflag && len > Mflag)
		len = Mflag;
	if(pcap){
		/* pcap needs a link-layer header.  this will fake one.  len is the
		 * packet length reported to pcap. */
		if (!proto_is_link_layer(root)) {
			hdrlen += Fakeethhdrlen;
			len += Fakeethhdrlen;
		}
		goo = (struct pcap_pkthdr*)(ps - hdrlen);
		goo->ts_sec = pkttime / 1000000000;
		goo->ts_nsec = pkttime % 1000000000;
		goo->caplen = len;
		goo->len = len;
		if (!proto_is_link_layer(root)) {
			fake_eth = (char*)goo + Pcaphdrlen;
			memcpy(fake_eth, fake_ethernet_header, Fakeethhdrlen);
		}
		write(1, goo, len + Pcaphdrlen);
	} else {
		hnputs(ps-10, len);
		hnputl(ps-8, pkttime>>32);
		hnputl(ps-4, pkttime);
		write(1, ps-10, len+10);
	}
}

/*
 *  format and print a packet
 */
void
printpkt(char *p, char *e, uint8_t *ps, uint8_t *pe)
{
	Msg m;
	uint32_t dt;
	ssize_t ret;
	size_t sofar, amt;

	dt = (pkttime-starttime)/1000000LL;
	m.p = seprint(p, e, "%6.6lu ms ", dt);
	m.ps = ps;
	m.pe = pe;
	m.e = e;
	m.pr = root;
	while(m.p < m.e){
		if(!sflag)
			m.p = seprint(m.p, m.e, "\n\t");
		m.p = seprint(m.p, m.e, "%s(", m.pr->name);
		if((*m.pr->seprint)(&m) < 0){
			m.p = seprint(m.p, m.e, "TOO SHORT");
			m.ps = m.pe;
		}
		m.p = seprint(m.p, m.e, ")");
		if(m.pr == NULL || m.ps >= m.pe)
			break;
	}
	*m.p++ = '\n';

	sofar = 0;
	amt = m.p - p;
	while (amt - sofar) {
		ret = write(1, p + sofar, amt - sofar);
		if (ret < 0) {
			sysfatal( "Error writing to stdout: %r");
			break;
		}
		sofar += ret;
	}
}

Proto **xprotos;
int nprotos;

/* look up a protocol by its name */
Proto*
findproto(char *name)
{
	int i;

	for(i = 0; i < nprotos; i++)
		if(strcmp(xprotos[i]->name, name) == 0)
			return xprotos[i];
	return NULL;
}

/*
 *  add an undefined protocol to protos[]
 */
Proto*
addproto(char *name)
{
	Proto *pr;

	xprotos = realloc(xprotos, (nprotos+1)*sizeof(Proto*));
	pr = malloc(sizeof *pr);
	*pr = dump;
	pr->name = name;
	xprotos[nprotos++] = pr;
	return pr;
}

/*
 *  build a graph of protocols, this could easily be circular.  This
 *  links together all the multiplexing in the protocol modules.
 */
void
mkprotograph(void)
{
	Proto **l;
	Proto *pr;
	Mux *m;

	/* copy protos into a reallocable area */
	for(nprotos = 0; protos[nprotos] != NULL; nprotos++)
		;
	xprotos = malloc(nprotos*sizeof(Proto*));
	memmove(xprotos, protos, nprotos*sizeof(Proto*));

	for(l = protos; *l != NULL; l++){
		pr = *l;
		for(m = pr->mux; m != NULL && m->name != NULL; m++){
			m->pr = findproto(m->name);
			if(m->pr == NULL)
				m->pr = addproto(m->name);
		}
	}
}

/*
 *  add in a protocol node
 */
static Filter*
addnode(Filter *f, Proto *pr)
{
	Filter *nf;
	nf = newfilter();
	nf->pr = pr;
	nf->s = pr->name;
	nf->l = f;
	nf->op = WORD;
	return nf;
}

/*
 *  recurse through the protocol graph adding missing nodes
 *  to the filter if we reach the filter's protocol
 */
static Filter*
_fillin(Filter *f, Proto *last, int depth)
{
	Mux *m;
	Filter *nf;

	if(depth-- <= 0)
		return NULL;

	for(m = last->mux; m != NULL && m->name != NULL; m++){
		if(m->pr == NULL)
			continue;
		if(f->pr == m->pr)
			return f;
		nf = _fillin(f, m->pr, depth);
		if(nf != NULL)
			return addnode(nf, m->pr);
	}
	return NULL;
}

static Filter*
fillin(Filter *f, Proto *last)
{
	int i;
	Filter *nf;

	/* hack to make sure top level node is the root */
	if(last == NULL){
		if(f->pr == root)
			return f;
		f = fillin(f, root);
		if(f == NULL)
			return NULL;
		return addnode(f, root);
	}

	/* breadth first search though the protocol graph */
	nf = f;
	for(i = 1; i < 20; i++){
		nf = _fillin(f, last, i);
		if(nf != NULL)
			break;
	}
	return nf;
}

/*
 *  massage tree so that all paths from the root to a leaf
 *  contain a filter node for each header.
 *
 *  also, set f->pr where possible
 */
Filter*
complete(Filter *f, Proto *last)
{
	Proto *pr;

	if(f == NULL)
		return f;

	/* do a depth first traversal of the filter tree */
	switch(f->op){
	case '!':
		f->l = complete(f->l, last);
		break;
	case LAND:
	case LOR:
		f->l = complete(f->l, last);
		f->r = complete(f->r, last);
		break;
	case '=':
		break;
	case WORD:
		pr = findproto(f->s);
		f->pr = pr;
		if(pr == NULL){
			if(f->l != NULL){
				fprintf(stderr, "%s unknown proto, ignoring params\n",
					f->s);
				f->l = NULL;
			}
		} else {
			f->l = complete(f->l, pr);
			f = fillin(f, last);
			if(f == NULL)
				sysfatal( "internal error: can't get to %s", pr->name);
		}
		break;
	}
	return f;
}

/*
 *  merge common nodes under | and & moving the merged node
 *  above the | or &.
 *
 *  do some constant foldong, e.g. `true & x' becomes x and
 *  'true | x' becomes true.
 */
static int changed;

static Filter*
_optimize(Filter *f)
{
	Filter *l;

	if(f == NULL)
		return f;

	switch(f->op){
	case '!':
		/* is child also a not */
		if(f->l->op == '!'){
			changed = 1;
			return f->l->l;
		}
		break;
	case LOR:
		/* are two children the same protocol? */
		if(f->l->op != f->r->op || f->r->op != WORD
		|| f->l->pr != f->r->pr || f->l->pr == NULL)
			break;	/* no optimization */

		changed = 1;

		/* constant folding */
		/* if either child is childless, just return that */
		if(f->l->l == NULL)
			return f->l;
		else if(f->r->l == NULL)
			return f->r;

		/* move the common node up, thow away one node */
		l = f->l;
		f->l = l->l;
		f->r = f->r->l;
		l->l = f;
		return l;
	case LAND:
		/* are two children the same protocol? */
		if(f->l->op != f->r->op || f->r->op != WORD
		|| f->l->pr != f->r->pr || f->l->pr == NULL)
			break;	/* no optimization */

		changed = 1;

		/* constant folding */
		/* if either child is childless, ignore it */
		if(f->l->l == NULL)
			return f->r;
		else if(f->r->l == NULL)
			return f->l;

		/* move the common node up, thow away one node */
		l = f->l;
		f->l = _optimize(l->l);
		f->r = _optimize(f->r->l);
		l->l = f;
		return l;
	}
	f->l = _optimize(f->l);
	f->r = _optimize(f->r);
	return f;
}

Filter*
optimize(Filter *f)
{
	do{
		changed = 0;
		f = _optimize(f);
	}while(changed);

	return f;
}

/*
 *  find any top level nodes that aren't the root
 */
int
findbogus(Filter *f)
{
	int rv;

	if(f->op != WORD){
		rv = findbogus(f->l);
		if(f->r)
			rv |= findbogus(f->r);
		return rv;
	} else if(f->pr != root){
		fprintf(stderr, "bad top-level protocol: %s\n", f->s);
		return 1;
	}
	return 0;
}

/*
 *  compile the filter
 */
static void
_compile(Filter *f, Proto *last)
{
	if(f == NULL)
		return;

	switch(f->op){
	case '!':
		_compile(f->l, last);
		break;
	case LOR:
	case LAND:
		_compile(f->l, last);
		_compile(f->r, last);
		break;
	case WORD:
		if(last != NULL){
			if(last->compile == NULL)
				sysfatal( "unknown %s subprotocol: %s", f->pr->name, f->s);
			(*last->compile)(f);
		}
		if(f->l)
			_compile(f->l, f->pr);
		break;
	case '=':
		if(last == NULL)
			sysfatal( "internal error: compilewalk: badly formed tree");
		
		if(last->compile == NULL)
			sysfatal( "unknown %s field: %s", f->pr->name, f->s);
		(*last->compile)(f);
		break;
	default:
		sysfatal( "internal error: compilewalk op: %d", f->op);
	}
}

Filter*
compile(Filter *f)
{
	if(f == NULL)
		return f;

	/* fill in the missing header filters */
	f = complete(f, NULL);

	/* constant folding */
	f = optimize(f);
	if(!toflag)
		printfilter(f, "after optimize");

	/* protocol specific compilations */
	_compile(f, NULL);

	/* at this point, the root had better be the root proto */
	if(findbogus(f)){
		fprintf(stderr, "bogus filter\n");
		exit(1);
	}

	return f;
}

/*
 *  parse a byte array
 */
int
parseba(uint8_t *to, char *from)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for(i = 0; i < 16; i++){
		if(*p == 0)
			return -1;
		nip[0] = *p++;
		if(*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
	}
	return i;
}

/*
 *  compile WORD = WORD, becomes a single node with a subop
 */
void
compile_cmp(char *proto, Filter *f, Field *fld)
{
#if 0       
	uint8_t x[IPaddrlen];
	char *v;
#endif
	if(f->op != '=')
		sysfatal( "internal error: compile_cmp %s: not a cmp", proto);

	for(; fld->name != NULL; fld++){
		if(strcmp(f->l->s, fld->name) == 0){
			f->op = WORD;
			f->subop = fld->subop;
			switch(fld->ftype){
			case Fnum:
				f->ulv = atoi(f->r->s);
				break;
			case Fether:
			case Fv4ip:
			case Fv6ip:
				sysfatal("Sorry, not supported yet");
				break;
#if 0
			case Fether:
				v = csgetvalue(NULL, "sys", (char*)f->r->s,
					"ether", 0);
				if(v){
					parseether(f->a, v);
					free(v);
				} else
					parseether(f->a, f->r->s);
				break;
			case Fv4ip:
				v = csgetvalue(NULL, "sys", (char*)f->r->s,
					"ip", 0);
				if(v){
					f->ulv = parseip(x, v);
					free(v);
				}else
					f->ulv = parseip(x, f->r->s);
				break;
			case Fv6ip:
				v = csgetvalue(NULL, "sys", (char*)f->r->s,
					"ipv6", 0);
				if(v){
					parseip(f->a, v);
					free(v);
				}else
					parseip(f->a, f->r->s);
				break;
#endif
			case Fba:
				parseba(f->a, f->r->s);
				break;
			default:
				sysfatal( "internal error: compile_cmp %s: %d",
					proto, fld->ftype);
			}
			f->l = f->r = NULL;
			return;
		}
	}
	sysfatal( "unknown %s field in: %s = %s", proto, f->l->s, f->r->s);
}

void
_pf(Filter *f)
{
	char *s;

	if(f == NULL)
		return;

	s = NULL;
	switch(f->op){
	case '!':
		fprintf(stderr, "!");
		_pf(f->l);
		break;
	case WORD:
		fprintf(stderr, "%s", f->s);
		if(f->l != NULL){
			fprintf(stderr, "(");
			_pf(f->l);
			fprintf(stderr, ")");
		}
		break;
	case LAND:
		s = "&&";
		goto print;
	case LOR:
		s = "||";
		goto print;
	case '=':
	print:
		_pf(f->l);
		if(s)
			fprintf(stderr, " %s ", s);
		else
			fprintf(stderr, " %c ", f->op);
		_pf(f->r);
		break;
	default:
		fprintf(stderr, "???");
		break;
	}
}

void
printfilter(Filter *f, char *tag)
{
	fprintf(stderr, "%s: ", tag);
	_pf(f);
	fprintf(stderr, "\n");
}

void
cat(void)
{
	char buf[1024];
	int n;
	
	while((n = read(0, buf, sizeof buf)) > 0)
		write(1, buf, n);
}

static int fd1 = -1;
void
startmc(void)
{
	int p[2];
	
	if(fd1 == -1)
		fd1 = dup(1);
	
	if(pipe(p) < 0)
		return;
	switch(fork()){
	case -1:
		return;
	default:
		close(p[0]);
		dup2(p[1], 1);
		if(p[1] != 1)
			close(p[1]);
		return;
	case 0:
		close(p[1]);
		dup2(p[0], 0);
		if(p[0] != 0)
			close(p[0]);
		execl("/bin/mc", "mc", NULL);
		cat();
		_exit(0);
	}
}

void
stopmc(void)
{
	close(1);
	dup2(fd1, 1);
	wait(NULL);
}

void
printhelp(char *name)
{
	int len;
	Proto *pr, **l;
	Mux *m;
	Field *f;
	char fmt[40];
	
	if(name == NULL){
		printf("protocols:\n");
		startmc();
		for(l=protos; (pr=*l) != NULL; l++)
			printf("  %s\n", pr->name);
		stopmc();
		return;
	}
	
	pr = findproto(name);
	if(pr == NULL){
		printf("unknown protocol %s\n", name);
		return;
	}
	
	if(pr->field){
		printf("%s's filter attributes:\n", pr->name);
		len = 0;
		for(f=pr->field; f->name; f++)
			if(len < strlen(f->name))
				len = strlen(f->name);
		startmc();
		for(f=pr->field; f->name; f++)
			printf("  %-*s - %s\n", len, f->name, f->help);
		stopmc();
	}
	if(pr->mux){
		printf("%s's subprotos:\n", pr->name);
		startmc();
		snprintf(fmt, sizeof fmt, "  %s %%s\n", pr->valfmt);
		for(m=pr->mux; m->name != NULL; m++)
			printf(fmt, m->val, m->name);
		stopmc();
	}
}

/*
 *  demultiplex to next prototol header
 */
void
demux(Mux *mx, uint32_t val1, uint32_t val2, Msg *m, Proto *def)
{
	m->pr = def;
	for(mx = mx; mx->name != NULL; mx++){
		if(val1 == mx->val || val2 == mx->val){
			m->pr = mx->pr;
			break;
		}
	}
}

/*
 *  default framer just assumes the input packet is
 *  a single read
 */
int
defaultframer(int fd, uint8_t *pkt, int pktlen)
{
	return read(fd, pkt, pktlen);
}

/* this is gross but I can't deal with yacc nonsense just now. */
void yyerror(void)
{
	sysfatal( "yyerror");
}
