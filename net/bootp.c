/*
 *	Based on LiMon - BOOTP.
 *
 *	Copyright 1994, 1995, 2000 Neil Russell.
 *	(See License)
 *	Copyright 2000 Roland Borde
 *	Copyright 2000 Paolo Scaffardi
 *	Copyright 2000-2004 Wolfgang Denk, wd@denx.de
 */
#include <common.h>
#include <command.h>
#include <net.h>
#include "bootp.h"
#include "tftp.h"
#include "nfs.h"
#ifdef CONFIG_STATUS_LED
#include <status_led.h>
#endif
#ifdef CONFIG_BOOTP_RANDOM_DELAY
#include "net_rand.h"
#endif

#define BOOTP_VENDOR_MAGIC	0x63825363	/* RFC1048 Magic Cookie */

#define TIMEOUT		10000UL	/* Milliseconds before trying BOOTP again */
#ifndef CONFIG_NET_RETRY_COUNT
# define TIMEOUT_COUNT	5		/* # of timeouts before giving up */
#else
# define TIMEOUT_COUNT	(CONFIG_NET_RETRY_COUNT)
#endif

#define PORT_BOOTPS	67		/* BOOTP server UDP port */
#define PORT_BOOTPC	68		/* BOOTP client UDP port */

#ifndef CONFIG_DHCP_MIN_EXT_LEN		/* minimal length of extension list */
#define CONFIG_DHCP_MIN_EXT_LEN 64
#endif

ulong		BootpID;
ulong		BootpIDlist[TIMEOUT_COUNT];
int		BootpTry;
extern unsigned dhcpserver;
IPaddr_t	NetUSBdevIP;
IPaddr_t	NetUSBhostIP;
IPaddr_t	NetUSBnetmaskIP;

#if defined(CONFIG_CMD_DHCP)
static dhcp_state_t dhcp_state = INIT;
static unsigned long dhcp_leasetime;
static IPaddr_t NetDHCPServerIP;
static void DhcpHandler(uchar *pkt, unsigned dest, IPaddr_t sip, unsigned src,
			unsigned len);

/* For Debug */
#if 0
static char *dhcpmsg2str(int type)
{
	switch (type) {
	case 1:	 return "DHCPDISCOVER"; break;
	case 2:	 return "DHCPOFFER";	break;
	case 3:	 return "DHCPREQUEST";	break;
	case 4:	 return "DHCPDECLINE";	break;
	case 5:	 return "DHCPACK";	break;
	case 6:	 return "DHCPNACK";	break;
	case 7:	 return "DHCPRELEASE";	break;
	default: return "UNKNOWN/INVALID MSG TYPE"; break;
	}
}
#endif
#endif

static int BootpIDCheck(ulong ID)
{
	int i;

	for (i = 0; i < BootpTry; i++)
	{
		if (BootpIDlist[i] == ID)
		{
			BootpID = ID;
			debug("ID match: %lx\n", ID);
			return 0;
		}
	}
	return 1;
}

static int BootpCheckPkt(uchar *pkt, unsigned dest, unsigned src, unsigned len)
{
	struct Bootp_t *bp = (struct Bootp_t *) pkt;
	int retval = 0;

	if (dhcpserver) {
		if (dest != PORT_BOOTPS || src != PORT_BOOTPC)
			retval = -1;
	}
	else if (dest != PORT_BOOTPC || src != PORT_BOOTPS)
		retval = -1;
	else if (len < sizeof(struct Bootp_t) - OPT_FIELD_SIZE)
		retval = -2;
	else if (bp->bp_op != OP_BOOTREQUEST &&
			bp->bp_op != OP_BOOTREPLY &&
			bp->bp_op != DHCP_OFFER &&
			bp->bp_op != DHCP_DISCOVER &&
			bp->bp_op != DHCP_ACK &&
			bp->bp_op != DHCP_NAK)
		retval = -3;
	else if (bp->bp_htype != HWT_ETHER)
		retval = -4;
	else if (bp->bp_hlen != HWL_ETHER)
		retval = -5;
	else if (BootpIDCheck(NetReadLong((ulong *) &bp->bp_id)) == 1)
		retval = -6;

	debug("Filtering pkt = %d\n", retval);

	return retval;
}

/*
 * Copy parameters of interest from BOOTP_REPLY/DHCP_OFFER packet
 */
static void BootpCopyNetParams(struct Bootp_t *bp)
{
#if !defined(CONFIG_BOOTP_SERVERIP)
	IPaddr_t tmp_ip;

	NetCopyIP(&tmp_ip, &bp->bp_siaddr);
	if (tmp_ip != 0)
		NetCopyIP(&NetServerIP, &bp->bp_siaddr);
	memcpy(NetServerEther, ((struct ethernet_hdr *)NetRxPacket)->et_src, 6);
#endif
	NetCopyIP(&NetOurIP, &bp->bp_yiaddr);
	if (strlen(bp->bp_file) > 0)
		copy_filename(BootFile, bp->bp_file, sizeof(BootFile));

	debug("Bootfile: %s\n", BootFile);

	/* Propagate to environment:
	 * don't delete exising entry when BOOTP / DHCP reply does
	 * not contain a new value
	 */
	if (*BootFile)
		setenv("bootfile", BootFile);
}

static int truncate_sz(const char *name, int maxlen, int curlen)
{
	if (curlen >= maxlen) {
		printf("*** WARNING: %s is too long (%d - max: %d)"
			" - truncated\n", name, curlen, maxlen);
		curlen = maxlen - 1;
	}
	return curlen;
}

#if !defined(CONFIG_CMD_DHCP)

static void BootpVendorFieldProcess(u8 *ext)
{
	int size = *(ext + 1);

	debug("[BOOTP] Processing extension %d... (%d bytes)\n", *ext,
		*(ext + 1));

	NetBootFileSize = 0;

	switch (*ext) {
		/* Fixed length fields */
	case 1:			/* Subnet mask */
		if (NetOurSubnetMask == 0)
			NetCopyIP(&NetOurSubnetMask, (IPaddr_t *) (ext + 2));
		break;
	case 2:			/* Time offset - Not yet supported */
		break;
		/* Variable length fields */
	case 3:			/* Gateways list */
		if (NetOurGatewayIP == 0)
			NetCopyIP(&NetOurGatewayIP, (IPaddr_t *) (ext + 2));
		break;
	case 4:			/* Time server - Not yet supported */
		break;
	case 5:			/* IEN-116 name server - Not yet supported */
		break;
	case 6:
		if (NetOurDNSIP == 0)
			NetCopyIP(&NetOurDNSIP, (IPaddr_t *) (ext + 2));
#if defined(CONFIG_BOOTP_DNS2)
		if ((NetOurDNS2IP == 0) && (size > 4))
			NetCopyIP(&NetOurDNS2IP, (IPaddr_t *) (ext + 2 + 4));
#endif
		break;
	case 7:			/* Log server - Not yet supported */
		break;
	case 8:			/* Cookie/Quote server - Not yet supported */
		break;
	case 9:			/* LPR server - Not yet supported */
		break;
	case 10:		/* Impress server - Not yet supported */
		break;
	case 11:		/* RPL server - Not yet supported */
		break;
	case 12:		/* Host name */
		if (NetOurHostName[0] == 0) {
			size = truncate_sz("Host Name",
				sizeof(NetOurHostName), size);
			memcpy(&NetOurHostName, ext + 2, size);
			NetOurHostName[size] = 0;
		}
		break;
	case 13:		/* Boot file size */
		if (size == 2)
			NetBootFileSize = ntohs(*(ushort *) (ext + 2));
		else if (size == 4)
			NetBootFileSize = ntohl(*(ulong *) (ext + 2));
		break;
	case 14:		/* Merit dump file - Not yet supported */
		break;
	case 15:		/* Domain name - Not yet supported */
		break;
	case 16:		/* Swap server - Not yet supported */
		break;
	case 17:		/* Root path */
		if (NetOurRootPath[0] == 0) {
			size = truncate_sz("Root Path",
				sizeof(NetOurRootPath), size);
			memcpy(&NetOurRootPath, ext + 2, size);
			NetOurRootPath[size] = 0;
		}
		break;
	case 18:		/* Extension path - Not yet supported */
		/*
		 * This can be used to send the information of the
		 * vendor area in another file that the client can
		 * access via TFTP.
		 */
		break;
		/* IP host layer fields */
	case 40:		/* NIS Domain name */
		if (NetOurNISDomain[0] == 0) {
			size = truncate_sz("NIS Domain Name",
				sizeof(NetOurNISDomain), size);
			memcpy(&NetOurNISDomain, ext + 2, size);
			NetOurNISDomain[size] = 0;
		}
		break;
#if defined(CONFIG_CMD_SNTP) && defined(CONFIG_BOOTP_NTPSERVER)
	case 42:	/* NTP server IP */
		NetCopyIP(&NetNtpServerIP, (IPaddr_t *) (ext + 2));
		break;
#endif
		/* Application layer fields */
	case 43:		/* Vendor specific info - Not yet supported */
		/*
		 * Binary information to exchange specific
		 * product information.
		 */
		break;
		/* Reserved (custom) fields (128..254) */
	}
}

static void BootpVendorProcess(u8 *ext, int size)
{
	u8 *end = ext + size;

	debug("[BOOTP] Checking extension (%d bytes)...\n", size);

	while ((ext < end) && (*ext != 0xff)) {
		if (*ext == 0) {
			ext++;
		} else {
			u8 *opt = ext;

			ext += ext[1] + 2;
			if (ext <= end)
				BootpVendorFieldProcess(opt);
		}
	}

	debug("[BOOTP] Received fields:\n");
	if (NetOurSubnetMask)
		debug("NetOurSubnetMask : %pI4\n", &NetOurSubnetMask);

	if (NetOurGatewayIP)
		debug("NetOurGatewayIP	: %pI4", &NetOurGatewayIP);

	if (NetBootFileSize)
		debug("NetBootFileSize : %d\n", NetBootFileSize);

	if (NetOurHostName[0])
		debug("NetOurHostName  : %s\n", NetOurHostName);

	if (NetOurRootPath[0])
		debug("NetOurRootPath  : %s\n", NetOurRootPath);

	if (NetOurNISDomain[0])
		debug("NetOurNISDomain : %s\n", NetOurNISDomain);

	if (NetBootFileSize)
		debug("NetBootFileSize: %d\n", NetBootFileSize);

#if defined(CONFIG_CMD_SNTP) && defined(CONFIG_BOOTP_NTPSERVER)
	if (NetNtpServerIP)
		debug("NetNtpServerIP : %pI4\n", &NetNtpServerIP);
#endif
}

/*
 *	Handle a BOOTP received packet.
 */
static void
BootpHandler(uchar *pkt, unsigned dest, IPaddr_t sip, unsigned src,
	     unsigned len)
{
	struct Bootp_t *bp;

	debug("got BOOTP packet (src=%d, dst=%d, len=%d want_len=%zu)\n",
		src, dest, len, sizeof(struct Bootp_t));

	bp = (struct Bootp_t *)pkt;

	/* Filter out pkts we don't want */
	if (BootpCheckPkt(pkt, dest, src, len))
		return;

	/*
	 *	Got a good BOOTP reply.	 Copy the data into our variables.
	 */
#ifdef CONFIG_STATUS_LED
	status_led_set(STATUS_LED_BOOT, STATUS_LED_OFF);
#endif

	BootpCopyNetParams(bp);		/* Store net parameters from reply */

	/* Retrieve extended information (we must parse the vendor area) */
	if (NetReadLong((ulong *)&bp->bp_vend[0]) == htonl(BOOTP_VENDOR_MAGIC))
		BootpVendorProcess((uchar *)&bp->bp_vend[4], len);

	NetSetTimeout(0, (thand_f *)0);
	bootstage_mark_name(BOOTSTAGE_ID_BOOTP_STOP, "bootp_stop");

	debug("Got good BOOTP\n");

	net_auto_load();
}
#endif

/*
 *	Timeout on BOOTP/DHCP request.
 */
static void
BootpTimeout(void)
{
	if (BootpTry >= TIMEOUT_COUNT) {
#ifdef CONFIG_BOOTP_MAY_FAIL
		puts("\nRetry count exceeded\n");
		net_set_state(NETLOOP_FAIL);
#else
		puts("\nRetry count exceeded; starting again\n");
		NetStartAgain();
#endif
	} else {
		NetSetTimeout(TIMEOUT, BootpTimeout);
		BootpRequest();
	}
}

#define put_vci(e, str)						\
	do {							\
		size_t vci_strlen = strlen(str);		\
		*e++ = 60;	/* Vendor Class Identifier */	\
		*e++ = vci_strlen;				\
		memcpy(e, str, vci_strlen);			\
		e += vci_strlen;				\
	} while (0)

/*
 *	Initialize BOOTP extension fields in the request.
 */
#if defined(CONFIG_CMD_DHCP)
static int DhcpExtended(u8 *e, int message_type, IPaddr_t ServerID,
			IPaddr_t RequestedIP)
{
	u8 *start = e;
	u8 *cnt;
#if defined(CONFIG_BOOTP_PXE)
	char *uuid;
	u16 clientarch;
#endif

#if defined(CONFIG_BOOTP_VENDOREX)
	u8 *x;
#endif
#if defined(CONFIG_BOOTP_SEND_HOSTNAME)
	char *hostname;
#endif

	*e++ = 99;		/* RFC1048 Magic Cookie */
	*e++ = 130;
	*e++ = 83;
	*e++ = 99;

	*e++ = 53;		/* DHCP Message Type */
	*e++ = 1;
	*e++ = message_type;

	*e++ = 57;		/* Maximum DHCP Message Size */
	*e++ = 2;
	*e++ = (576 - 312 + OPT_FIELD_SIZE) >> 8;
	*e++ = (576 - 312 + OPT_FIELD_SIZE) & 0xff;

	if (ServerID) {
		int tmp = ntohl(ServerID);

		*e++ = 54;	/* ServerID */
		*e++ = 4;
		*e++ = tmp >> 24;
		*e++ = tmp >> 16;
		*e++ = tmp >> 8;
		*e++ = tmp & 0xff;
	}

	if (RequestedIP) {
		int tmp = ntohl(RequestedIP);

		*e++ = 50;	/* Requested IP */
		*e++ = 4;
		*e++ = tmp >> 24;
		*e++ = tmp >> 16;
		*e++ = tmp >> 8;
		*e++ = tmp & 0xff;
	}
#if defined(CONFIG_BOOTP_SEND_HOSTNAME)
	hostname = getenv("hostname");
	if (hostname) {
		int hostnamelen = strlen(hostname);

		*e++ = 12;	/* Hostname */
		*e++ = hostnamelen;
		memcpy(e, hostname, hostnamelen);
		e += hostnamelen;
	}
#endif

#if defined(CONFIG_BOOTP_PXE)
	clientarch = CONFIG_BOOTP_PXE_CLIENTARCH;
	*e++ = 93;	/* Client System Architecture */
	*e++ = 2;
	*e++ = (clientarch >> 8) & 0xff;
	*e++ = clientarch & 0xff;

	*e++ = 94;	/* Client Network Interface Identifier */
	*e++ = 3;
	*e++ = 1;	/* type field for UNDI */
	*e++ = 0;	/* major revision */
	*e++ = 0;	/* minor revision */

	uuid = getenv("pxeuuid");

	if (uuid) {
		if (uuid_str_valid(uuid)) {
			*e++ = 97;	/* Client Machine Identifier */
			*e++ = 17;
			*e++ = 0;	/* type 0 - UUID */

			uuid_str_to_bin(uuid, e);
			e += 16;
		} else {
			printf("Invalid pxeuuid: %s\n", uuid);
		}
	}
#endif

#ifdef CONFIG_BOOTP_VCI_STRING
	put_vci(e, CONFIG_BOOTP_VCI_STRING);
#endif

#if defined(CONFIG_BOOTP_VENDOREX)
	x = dhcp_vendorex_prep(e);
	if (x)
		return x - start;
#endif

	*e++ = 55;		/* Parameter Request List */
	 cnt = e++;		/* Pointer to count of requested items */
	*cnt = 0;
#if defined(CONFIG_BOOTP_SUBNETMASK)
	*e++  = 1;		/* Subnet Mask */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_TIMEOFFSET)
	*e++  = 2;
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_GATEWAY)
	*e++  = 3;		/* Router Option */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_DNS)
	*e++  = 6;		/* DNS Server(s) */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_HOSTNAME)
	*e++  = 12;		/* Hostname */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_BOOTFILESIZE)
	*e++  = 13;		/* Boot File Size */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_BOOTPATH)
	*e++  = 17;		/* Boot path */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_NISDOMAIN)
	*e++  = 40;		/* NIS Domain name request */
	*cnt += 1;
#endif
#if defined(CONFIG_BOOTP_NTPSERVER)
	*e++  = 42;
	*cnt += 1;
#endif
	/* no options, so back up to avoid sending an empty request list */
	if (*cnt == 0)
		e -= 2;

	*e++  = 255;		/* End of the list */

	/* Pad to minimal length */
#ifdef	CONFIG_DHCP_MIN_EXT_LEN
	while ((e - start) < CONFIG_DHCP_MIN_EXT_LEN)
		*e++ = 0;
#endif

	return e - start;
}

#else
/*
 * Warning: no field size check - change CONFIG_BOOTP_* at your own risk!
 */
static int BootpExtended(u8 *e)
{
	u8 *start = e;

	*e++ = 99;		/* RFC1048 Magic Cookie */
	*e++ = 130;
	*e++ = 83;
	*e++ = 99;

#if defined(CONFIG_CMD_DHCP)
	*e++ = 53;		/* DHCP Message Type */
	*e++ = 1;
	*e++ = DHCP_DISCOVER;

	*e++ = 57;		/* Maximum DHCP Message Size */
	*e++ = 2;
	*e++ = (576 - 312 + OPT_FIELD_SIZE) >> 16;
	*e++ = (576 - 312 + OPT_FIELD_SIZE) & 0xff;
#endif

#if defined(CONFIG_BOOTP_VCI_STRING) || \
	(defined(CONFIG_SPL_BUILD) && defined(CONFIG_SPL_NET_VCI_STRING))
#ifdef CONFIG_SPL_BUILD
	put_vci(e, CONFIG_SPL_NET_VCI_STRING);
#else
	put_vci(e, CONFIG_BOOTP_VCI_STRING);
#endif
#endif

#if defined(CONFIG_BOOTP_SUBNETMASK)
	*e++ = 1;		/* Subnet mask request */
	*e++ = 4;
	e   += 4;
#endif

#if defined(CONFIG_BOOTP_GATEWAY)
	*e++ = 3;		/* Default gateway request */
	*e++ = 4;
	e   += 4;
#endif

#if defined(CONFIG_BOOTP_DNS)
	*e++ = 6;		/* Domain Name Server */
	*e++ = 4;
	e   += 4;
#endif

#if defined(CONFIG_BOOTP_HOSTNAME)
	*e++ = 12;		/* Host name request */
	*e++ = 32;
	e   += 32;
#endif

#if defined(CONFIG_BOOTP_BOOTFILESIZE)
	*e++ = 13;		/* Boot file size */
	*e++ = 2;
	e   += 2;
#endif

#if defined(CONFIG_BOOTP_BOOTPATH)
	*e++ = 17;		/* Boot path */
	*e++ = 32;
	e   += 32;
#endif

#if defined(CONFIG_BOOTP_NISDOMAIN)
	*e++ = 40;		/* NIS Domain name request */
	*e++ = 32;
	e   += 32;
#endif
#if defined(CONFIG_BOOTP_NTPSERVER)
	*e++ = 42;
	*e++ = 4;
	e   += 4;
#endif

	*e++ = 255;		/* End of the list */

	return e - start;
}
#endif

void
BootpRequest(void)
{
	uchar *pkt, *iphdr;
	struct Bootp_t *bp;
	int extlen, pktlen, iplen;
	int eth_hdr_size;
#ifdef CONFIG_BOOTP_RANDOM_DELAY
	ulong i, rand_ms;
#endif

	bootstage_mark_name(BOOTSTAGE_ID_BOOTP_START, "bootp_start");
#if defined(CONFIG_CMD_DHCP)
	dhcp_state = INIT;
#endif

#ifdef CONFIG_BOOTP_RANDOM_DELAY		/* Random BOOTP delay */
	if (BootpTry == 0)
		srand_mac();

	if (BootpTry <= 2)	/* Start with max 1024 * 1ms */
		rand_ms = rand() >> (22 - BootpTry);
	else		/* After 3rd BOOTP request max 8192 * 1ms */
		rand_ms = rand() >> 19;

	printf("Random delay: %ld ms...\n", rand_ms);
	for (i = 0; i < rand_ms; i++)
		udelay(1000); /*Wait 1ms*/

#endif	/* CONFIG_BOOTP_RANDOM_DELAY */

	printf("BOOTP broadcast %d\n", ++BootpTry);
	pkt = NetTxPacket;
	memset((void *)pkt, 0, PKTSIZE);

	eth_hdr_size = NetSetEther(pkt, NetBcastAddr, PROT_IP);
	pkt += eth_hdr_size;

	/*
	 * Next line results in incorrect packet size being transmitted,
	 * resulting in errors in some DHCP servers, reporting missing bytes.
	 * Size must be set in packet header after extension length has been
	 * determined.
	 * C. Hallinan, DS4.COM, Inc.
	 */
	/* net_set_udp_header(pkt, 0xFFFFFFFFL, PORT_BOOTPS, PORT_BOOTPC,
		sizeof (struct Bootp_t)); */
	iphdr = pkt;	/* We need this later for net_set_udp_header() */
	pkt += IP_UDP_HDR_SIZE;

	bp = (struct Bootp_t *)pkt;
	bp->bp_op = OP_BOOTREQUEST;
	bp->bp_htype = HWT_ETHER;
	bp->bp_hlen = HWL_ETHER;
	bp->bp_hops = 0;
	bp->bp_secs = htons(get_timer(0) / 1000);
	NetWriteIP(&bp->bp_ciaddr, 0);
	NetWriteIP(&bp->bp_yiaddr, 0);
	NetWriteIP(&bp->bp_siaddr, 0);
	NetWriteIP(&bp->bp_giaddr, 0);
	memcpy(bp->bp_chaddr, NetOurEther, 6);
	copy_filename(bp->bp_file, BootFile, sizeof(bp->bp_file));

	/* Request additional information from the BOOTP/DHCP server */
#if defined(CONFIG_CMD_DHCP)
	extlen = DhcpExtended((u8 *)bp->bp_vend, DHCP_DISCOVER, 0, 0);
#else
	extlen = BootpExtended((u8 *)bp->bp_vend);
#endif

	/*
	 *	Bootp ID is the lower 4 bytes of our ethernet address
	 *	plus the current time in ms.
	 */
	BootpID = ((ulong)NetOurEther[2] << 24)
		| ((ulong)NetOurEther[3] << 16)
		| ((ulong)NetOurEther[4] << 8)
		| (ulong)NetOurEther[5];
	BootpID += get_timer(0);
	BootpID	 = htonl(BootpID);
	BootpIDlist[BootpTry - 1] = BootpID;
	NetCopyLong(&bp->bp_id, &BootpID);

	/*
	 * Calculate proper packet lengths taking into account the
	 * variable size of the options field
	 */
	iplen = BOOTP_HDR_SIZE - OPT_FIELD_SIZE + extlen;
	pktlen = eth_hdr_size + IP_UDP_HDR_SIZE + iplen;
	net_set_udp_header(iphdr, 0xFFFFFFFFL, PORT_BOOTPS, PORT_BOOTPC, iplen);
	NetSetTimeout(SELECT_TIMEOUT, BootpTimeout);

#if defined(CONFIG_CMD_DHCP)
	dhcp_state = SELECTING;
	net_set_udp_handler(DhcpHandler);
#else
	net_set_udp_handler(BootpHandler);
#endif
	NetSendPacket(NetTxPacket, pktlen);
}

#if defined(CONFIG_CMD_DHCP)
static void DhcpOptionsProcess(uchar *popt, struct Bootp_t *bp)
{
	uchar *end = popt + BOOTP_HDR_SIZE;
	int oplen, size;
#if defined(CONFIG_CMD_SNTP) && defined(CONFIG_BOOTP_TIMEOFFSET)
	int *to_ptr;
#endif

	while (popt < end && *popt != 0xff) {
		oplen = *(popt + 1);
		switch (*popt) {
		case 1:
			NetCopyIP(&NetOurSubnetMask, (popt + 2));
			break;
#if defined(CONFIG_CMD_SNTP) && defined(CONFIG_BOOTP_TIMEOFFSET)
		case 2:		/* Time offset	*/
			to_ptr = &NetTimeOffset;
			NetCopyLong((ulong *)to_ptr, (ulong *)(popt + 2));
			NetTimeOffset = ntohl(NetTimeOffset);
			break;
#endif
		case 3:
			NetCopyIP(&NetOurGatewayIP, (popt + 2));
			break;
		case 6:
			NetCopyIP(&NetOurDNSIP, (popt + 2));
#if defined(CONFIG_BOOTP_DNS2)
			if (*(popt + 1) > 4)
				NetCopyIP(&NetOurDNS2IP, (popt + 2 + 4));
#endif
			break;
		case 12:
			size = truncate_sz("Host Name",
				sizeof(NetOurHostName), oplen);
			memcpy(&NetOurHostName, popt + 2, size);
			NetOurHostName[size] = 0;
			break;
		case 15:	/* Ignore Domain Name Option */
			break;
		case 17:
			size = truncate_sz("Root Path",
				sizeof(NetOurRootPath), oplen);
			memcpy(&NetOurRootPath, popt + 2, size);
			NetOurRootPath[size] = 0;
			break;
		case 28:	/* Ignore Broadcast Address Option */
			break;
#if defined(CONFIG_CMD_SNTP) && defined(CONFIG_BOOTP_NTPSERVER)
		case 42:	/* NTP server IP */
			NetCopyIP(&NetNtpServerIP, (popt + 2));
			break;
#endif
		case 51:
			NetCopyLong(&dhcp_leasetime, (ulong *) (popt + 2));
			break;
		case 53:	/* Ignore Message Type Option */
			break;
		case 54:
			NetCopyIP(&NetDHCPServerIP, (popt + 2));
			break;
		case 58:	/* Ignore Renewal Time Option */
			break;
		case 59:	/* Ignore Rebinding Time Option */
			break;
		case 66:	/* Ignore TFTP server name */
			break;
		case 67:	/* vendor opt bootfile */
			/*
			 * I can't use dhcp_vendorex_proc here because I need
			 * to write into the bootp packet - even then I had to
			 * pass the bootp packet pointer into here as the
			 * second arg
			 */
			size = truncate_sz("Opt Boot File",
					    sizeof(bp->bp_file),
					    oplen);
			if (bp->bp_file[0] == '\0' && size > 0) {
				/*
				 * only use vendor boot file if we didn't
				 * receive a boot file in the main non-vendor
				 * part of the packet - god only knows why
				 * some vendors chose not to use this perfectly
				 * good spot to store the boot file (join on
				 * Tru64 Unix) it seems mind bogglingly crazy
				 * to me
				 */
				printf("*** WARNING: using vendor "
					"optional boot file\n");
				memcpy(bp->bp_file, popt + 2, size);
				bp->bp_file[size] = '\0';
			}
			break;
		default:
#if defined(CONFIG_BOOTP_VENDOREX)
			if (dhcp_vendorex_proc(popt))
				break;
#endif
			printf("*** Unhandled DHCP Option in OFFER/ACK:"
				" %d\n", *popt);
			break;
		}
		popt += oplen + 2;	/* Process next option */
	}
}

static int DhcpMessageType(unsigned char *popt)
{
	if (NetReadLong((ulong *)popt) != htonl(BOOTP_VENDOR_MAGIC))
		return -1;

	popt += 4;
	while (*popt != 0xff) {
		if (*popt == 53)	/* DHCP Message Type */
			return *(popt + 2);
		popt += *(popt + 1) + 2;	/* Scan through all options */
	}
	return -1;
}

static void DhcpPrintPkt(struct Bootp_t *bp, int len)
{
#ifdef DEBUG
	int i;
	char *ptr = bp;

	for (i = 0; i < len; i++) {
		if ((i % 25) == 0)
			printf("\n");
		printf("%02X ", ptr[i]);
	}
	printf("\n");
#endif
}

static void DhcpRecvDiscoverPkt(struct Bootp_t *bp_discover, int len)
{
	uchar *pkt, *iphdr;
	struct Bootp_t *bp;
	int i, iplen, pktlen, eth_hdr_size;
	uchar *ptr = (uchar *) bp_discover;

	debug("DhcpRecvDiscoverPkt\n");

	DhcpPrintPkt(bp_discover, len);

/* Lets send out our DHCP OFFER
 * We really dont have to look at the options that were passed in
 * discover. We just return back 4 messages  (below in decimal)
 * 1. 53 = 0x35 (DHCP Message type) 1 (1 byte) X (DHCP Offer(2) or DHCP Ack(5))
 * 2. 54 = 0x36 (Server identifier) 4 (4 bytes) Server IP addresss
 * 3. 51 = 0x33 (IP address lease time) 4 (4 bytes) 85536 (24 hours)
 * 4. 1 (Subnet mask) 4 (4 bytes) 255.255.255.0
 */
	pkt = NetTxPacket;
	memset((void *)pkt, 0, PKTSIZE);

	eth_hdr_size = NetSetEther(pkt, NetBcastAddr, PROT_IP);
	pkt += eth_hdr_size;

	iphdr = pkt;	/* We'll need this later to set proper pkt size */
	pkt += IP_UDP_HDR_SIZE;

	bp = (struct Bootp_t *)pkt;
	bp->bp_op = OP_BOOTREPLY;
	bp->bp_htype = HWT_ETHER;
	bp->bp_hlen = HWL_ETHER;
	bp->bp_hops = 0;
	bp->bp_secs = bp_discover->bp_secs;

	memcpy(&bp->bp_chaddr, &bp_discover->bp_chaddr, 6);

	memcpy(&bp->bp_id, &bp_discover->bp_id, 4);

	NetCopyIP(&bp->bp_siaddr, &NetUSBdevIP); 
	NetCopyIP(&bp->bp_yiaddr, &NetUSBhostIP); 

	/* Copy the magic */
	memcpy(&bp->bp_vend, &bp_discover->bp_vend, 4);

	bp->bp_vend[4] = 0x35;
	bp->bp_vend[5] = 0x01;
	bp->bp_vend[6] = DHCP_OFFER;
	bp->bp_vend[7] = 0x36; /* DHCP Server IP */
	bp->bp_vend[8] = 0x04;
	NetCopyIP(&bp->bp_vend[9], &NetUSBdevIP);
	bp->bp_vend[13] = 0x33; /* Lease time */
	bp->bp_vend[14] = 0x04; 
	bp->bp_vend[15] = 0x00; /* 00 01 51 89 */
	bp->bp_vend[16] = 0x01;
	bp->bp_vend[17] = 0x51;
	bp->bp_vend[18] = 0x89;
	bp->bp_vend[19] = 0x01; /* subnet mask */
	bp->bp_vend[20] = 0x04;
	NetCopyIP(&bp->bp_vend[21], &NetUSBnetmaskIP);
	bp->bp_vend[25] = 0xFF; /* End option marker */

	iplen = BOOTP_HDR_SIZE - OPT_FIELD_SIZE + 26;
	debug("Sending DHCP Offer packet\n");
	DhcpPrintPkt(bp, iplen);
	pktlen = eth_hdr_size + IP_UDP_HDR_SIZE + iplen;
	net_set_udp_header(iphdr, 0xFFFFFFFFL, PORT_BOOTPC, PORT_BOOTPS, iplen);
	NetSendPacket(NetTxPacket, pktlen);
}

static void DhcpRecvRequestPkt(struct Bootp_t *bp_request, int len)
{
	uchar *pkt, *iphdr;
	struct Bootp_t *bp;
	int i, iplen, pktlen, eth_hdr_size;
	uchar *ptr = (uchar *) bp_request;

	debug("DhcpRecvRequestPkt\n");

	DhcpPrintPkt(bp_request, len);

	/* Lets send out our DHCP ACK */
	pkt = NetTxPacket;
	memset((void *)pkt, 0, PKTSIZE);

	eth_hdr_size = NetSetEther(pkt, NetBcastAddr, PROT_IP);
	pkt += eth_hdr_size;

	iphdr = pkt;	/* We'll need this later to set proper pkt size */
	pkt += IP_UDP_HDR_SIZE;

	bp = (struct Bootp_t *)pkt;
	bp->bp_op = OP_BOOTREPLY;
	bp->bp_htype = HWT_ETHER;
	bp->bp_hlen = HWL_ETHER;
	bp->bp_hops = 0;
	bp->bp_secs = bp_request->bp_secs;

	memcpy(&bp->bp_chaddr, &bp_request->bp_chaddr, 6);

	memcpy(&bp->bp_id, &bp_request->bp_id, 4);

	NetCopyIP(&bp->bp_siaddr, &NetUSBdevIP);
	NetCopyIP(&bp->bp_yiaddr, &NetUSBhostIP);

	/* Copy the magic */
	memcpy(&bp->bp_vend, &bp_request->bp_vend, 4);

	bp->bp_vend[4] = 0x35;
	bp->bp_vend[5] = 0x01;
	bp->bp_vend[6] = DHCP_ACK; 
	bp->bp_vend[7] = 0x36; /* DHCP Server IP */
	bp->bp_vend[8] = 0x04;
	NetCopyIP(&bp->bp_vend[9], &NetUSBdevIP);
	bp->bp_vend[13] = 0x33; /* Lease time */
	bp->bp_vend[14] = 0x04; 
	bp->bp_vend[15] = 0x00; /* 00 01 51 89 */
	bp->bp_vend[16] = 0x01;
	bp->bp_vend[17] = 0x51;
	bp->bp_vend[18] = 0x89;
	bp->bp_vend[19] = 0x01; /* subnet mask */
	bp->bp_vend[20] = 0x04;
	NetCopyIP(&bp->bp_vend[21], &NetUSBnetmaskIP);
	bp->bp_vend[25] = 0xFF; /* End option marker */

	iplen = BOOTP_HDR_SIZE - OPT_FIELD_SIZE + 26;
	debug("Sending DHCP Ack packet\n");
	DhcpPrintPkt(bp, iplen);
	pktlen = eth_hdr_size + IP_UDP_HDR_SIZE + iplen;
	net_set_udp_header(iphdr, 0xFFFFFFFFL, PORT_BOOTPC, PORT_BOOTPS, iplen);
	NetSendPacket(NetTxPacket, pktlen);
}

static void DhcpSendRequestPkt(struct Bootp_t *bp_offer)
{
	uchar *pkt, *iphdr;
	struct Bootp_t *bp;
	int pktlen, iplen, extlen;
	int eth_hdr_size;
	IPaddr_t OfferedIP;

	debug("DhcpSendRequestPkt: Sending DHCPREQUEST\n");
	pkt = NetTxPacket;
	memset((void *)pkt, 0, PKTSIZE);

	eth_hdr_size = NetSetEther(pkt, NetBcastAddr, PROT_IP);
	pkt += eth_hdr_size;

	iphdr = pkt;	/* We'll need this later to set proper pkt size */
	pkt += IP_UDP_HDR_SIZE;

	bp = (struct Bootp_t *)pkt;
	bp->bp_op = OP_BOOTREQUEST;
	bp->bp_htype = HWT_ETHER;
	bp->bp_hlen = HWL_ETHER;
	bp->bp_hops = 0;
	bp->bp_secs = htons(get_timer(0) / 1000);
	/* Do not set the client IP, your IP, or server IP yet, since it
	 * hasn't been ACK'ed by the server yet */

	/*
	 * RFC3046 requires Relay Agents to discard packets with
	 * nonzero and offered giaddr
	 */
	NetWriteIP(&bp->bp_giaddr, 0);

	memcpy(bp->bp_chaddr, NetOurEther, 6);

	/*
	 * ID is the id of the OFFER packet
	 */

	NetCopyLong(&bp->bp_id, &bp_offer->bp_id);

	/*
	 * Copy options from OFFER packet if present
	 */

	/* Copy offered IP into the parameters request list */
	NetCopyIP(&OfferedIP, &bp_offer->bp_yiaddr);
	extlen = DhcpExtended((u8 *)bp->bp_vend, DHCP_REQUEST,
		NetDHCPServerIP, OfferedIP);

	iplen = BOOTP_HDR_SIZE - OPT_FIELD_SIZE + extlen;
	pktlen = eth_hdr_size + IP_UDP_HDR_SIZE + iplen;
	net_set_udp_header(iphdr, 0xFFFFFFFFL, PORT_BOOTPS, PORT_BOOTPC, iplen);

#ifdef CONFIG_BOOTP_DHCP_REQUEST_DELAY
	udelay(CONFIG_BOOTP_DHCP_REQUEST_DELAY);
#endif	/* CONFIG_BOOTP_DHCP_REQUEST_DELAY */
	debug("Transmitting DHCPREQUEST packet: len = %d\n", pktlen);
	NetSendPacket(NetTxPacket, pktlen);
}

/*
 *	Handle DHCP received packets.
 */
static void
DhcpHandler(uchar *pkt, unsigned dest, IPaddr_t sip, unsigned src,
	    unsigned len)
{
	struct Bootp_t *bp = (struct Bootp_t *)pkt;

	/* Filter out pkts we don't want */
	if (BootpCheckPkt(pkt, dest, src, len))
		return;

	debug("DHCPHandler: got DHCP packet: (src=%d, dst=%d, len=%d) state:"
		" %d\n", src, dest, len, dhcp_state);

	switch (dhcp_state) {
	case SELECTING:
		/*
		 * Wait an appropriate time for any potential DHCPOFFER packets
		 * to arrive.  Then select one, and generate DHCPREQUEST
		 * response.  If filename is in format we recognize, assume it
		 * is a valid OFFER from a server we want.
		 */
		debug("DHCP: state=SELECTING bp_file: \"%s\"\n", bp->bp_file);
#ifdef CONFIG_SYS_BOOTFILE_PREFIX
		if (strncmp(bp->bp_file,
			    CONFIG_SYS_BOOTFILE_PREFIX,
			    strlen(CONFIG_SYS_BOOTFILE_PREFIX)) == 0) {
#endif	/* CONFIG_SYS_BOOTFILE_PREFIX */

			debug("TRANSITIONING TO REQUESTING STATE\n");
			dhcp_state = REQUESTING;

			if (NetReadLong((ulong *)&bp->bp_vend[0]) ==
						htonl(BOOTP_VENDOR_MAGIC))
				DhcpOptionsProcess((u8 *)&bp->bp_vend[4], bp);

			NetSetTimeout(TIMEOUT, BootpTimeout);
			DhcpSendRequestPkt(bp);
#ifdef CONFIG_SYS_BOOTFILE_PREFIX
		}
#endif	/* CONFIG_SYS_BOOTFILE_PREFIX */

		return;
		break;
	case REQUESTING:
		debug("DHCP State: REQUESTING\n");

		if (DhcpMessageType((u8 *)bp->bp_vend) == DHCP_ACK) {
			if (NetReadLong((ulong *)&bp->bp_vend[0]) ==
						htonl(BOOTP_VENDOR_MAGIC))
				DhcpOptionsProcess((u8 *)&bp->bp_vend[4], bp);
			/* Store net params from reply */
			BootpCopyNetParams(bp);
			dhcp_state = BOUND;
			printf("DHCP client bound to address %pI4\n",
				&NetOurIP);
			bootstage_mark_name(BOOTSTAGE_ID_BOOTP_STOP,
				"bootp_stop");

			net_auto_load();
			return;
		}
		break;
	case BOUND:
		/* DHCP client bound to address */
		break;
	case WAITFORDISCOVER:
		/* Got a DISCOVER packet - send response and set */
		/* state to WAITFORREQUEST */
		/* We can also get a direct REQUEST packet */
		if (DhcpMessageType((u8 *)bp->bp_vend) == DHCP_REQUEST)
		{
			debug("REQUEST packet received\n");
			DhcpRecvRequestPkt(bp, len);
			dhcpserver = 2;
			net_set_state(NETLOOP_SUCCESS);
			break;
		}
		if (DhcpMessageType((u8 *)bp->bp_vend) != DHCP_DISCOVER)
			break;
		debug("DISCOVER packet received\n");
		DhcpRecvDiscoverPkt(bp, len);
		dhcp_state = WAITFORREQUEST;
		break;

	case WAITFORREQUEST:
		/* Got a Request. Send an ACK packet */
		if (DhcpMessageType((u8 *)bp->bp_vend) != DHCP_REQUEST)
			break;
		debug("REQUEST packet received\n");
		DhcpRecvRequestPkt(bp, len);
		dhcpserver = 2;
		net_set_state(NETLOOP_SUCCESS);
		break;

	default:
		puts("DHCP: INVALID STATE\n");
		break;
	}

}

void DhcpRequest(void)
{
	BootpRequest();
}

/* Called from netconsole.c when dhcpserver is 1.
 * We set dhcpserver to 3 as we have timed out.
 */
static void dhcpserver_to_handler(void)
{
	debug("\n%s: DHCP Server time out\n", __func__);
	/* We set it to success so that it doesnt reinit the eth device */
	net_set_state(NETLOOP_SUCCESS);
	/* We set this to 3 to imply that a DHCP server failed. */
	dhcpserver = 3;
}

int DhcpServer(void)
{
	int retval = -1;

	net_init();

	NetUSBdevIP = string_to_ip(CONFIG_USBNET_DEV_IP);
	NetUSBhostIP = string_to_ip(CONFIG_USBNET_HOST_IP);
	NetUSBnetmaskIP = string_to_ip(CONFIG_USBNET_DEV_NETMASK);

	eth_set_last_protocol(DHCPSERVER);

	dhcp_state = WAITFORDISCOVER;
	net_set_udp_handler(DhcpHandler);

/*
 * Here we loop for some timeout to see if we do get a
 * DISCOVER packet. If we don't we just return -1.
 * We need to set timeout to about 10 seconds.
 */
	NetSetTimeout(10000, dhcpserver_to_handler);
	NetLoop(DHCPSERVER);
	if (dhcpserver == 3) {
		debug("No DHCP Request received\n");
		return -1;
	}

	return 0;
}
#endif	/* CONFIG_CMD_DHCP */
