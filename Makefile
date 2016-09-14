# Snoopy from harvey (from Plan 9)
# Rules: keep this simple. Make sure the gcc is in your path and nobody gets hurt.

### Build flags for all targets
#
CFLAGS          = -O2 -std=gnu99 -fno-stack-protector -fgnu89-inline -fPIC -static -fno-omit-frame-pointer -g -Iinclude -Wall
LDFLAGS          =
LDLIBS         = -L$(AKAROS)/install/x86_64-ucb-akaros/sysroot/usr/lib -lpthread -lbenchutil -lm -liplib -lndblib -lvmm -lbenchutil
DEST	= $(AKAROS)/kern/kfs/bin

### Build tools
# 
CC=x86_64-ucb-akaros-gcc
AR=x86_64-ucb-akaros-ar
ALL=snoopy
FILES= \
aoeata.c \
aoe.c \
aoecmd.c \
aoemask.c \
aoemd.c \
aoerr.c \
arp.c \
bootp.c \
cec.c \
dhcp.c \
dns.c \
dump.c \
eap.c \
eap_identity.c \
eapol.c \
eapol_key.c \
ether.c \
gre.c \
hdlc.c \
icmp6.c \
icmp.c \
il.c \
ip6.c \
ip.c \
main.c \
ninep.c \
ospf.c \
ppp.c \
ppp_ccp.c \
ppp_chap.c \
ppp_comp.c \
ppp_ipcp.c \
ppp_lcp.c \
pppoe_disc.c \
pppoe_sess.c \
rarp.c \
rc4keydesc.c \
rtcp.c \
rtp.c \
tcp.c \
ttls.c \
udp.c

all: $(ALL)
	scp $(ALL) skynet:

install: all
	echo "Installing $(ALL) in $(DEST)"
	cp $(ALL) $(DEST)

# compilers are fast. Just rebuild it each time.
snoopy: $(FILES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
clean:
	rm -f $(ALL) *.o


