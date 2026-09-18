/*
 * rs485ctl - configure kernel RS485 (TIOCSRS485) settings on a serial
 * port: enable automatic RTS direction control for half-duplex buses
 * (MAX485-style transceiver with DE/!RE wired to the UART's RTS).
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/serial.h>

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s DEV [options]\n"
		"  -g            get: print current settings and exit\n"
		"  -e            enable RS485 mode\n"
		"  -d            disable RS485 mode\n"
		"  -i            inverted RTS: released while driving\n"
		"  -n            normal RTS: asserted while driving\n"
		"  --send-delay MS   hold RTS MS after the last byte\n"
		"  --after-delay MS  RTS release time before RX (ms)\n"
		"\n"
		"example (PL011 + MAX485, DE wired to RTS):\n"
		"  stty -F /dev/ttyAMA0 9600\n"
		"  %s /dev/ttyAMA0 -e -n --send-delay 1 --after-delay 1\n",
		prog, prog);
	exit(2);
}

static void print_cfg(const char *dev, const struct serial_rs485 *rs)
{
	printf("%s: rs485 %s, RTS %s while driving, RTS %s after send, "
	       "delays before/after %u/%u ms\n",
	       dev,
	       (rs->flags & SER_RS485_ENABLED) ? "enabled" : "disabled",
	       (rs->flags & SER_RS485_RTS_ON_SEND) ? "asserted" : "released",
	       (rs->flags & SER_RS485_RTS_AFTER_SEND) ? "asserted" : "released",
	       rs->delay_rts_before_send, rs->delay_rts_after_send);
}

int main(int argc, char **argv)
{
	struct serial_rs485 rs, new;
	const char *dev;
	int fd, get_only = 0, change = 0;
	int enable = -1, invert = -1;
	unsigned int dsend = 0, dafter = 0, hsend = 0, hafter = 0;

	if (argc < 2)
		usage(argv[0]);

	dev = argv[1];

	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-g")) {
			get_only = 1;
		} else if (!strcmp(a, "-e")) {
			enable = 1;
		} else if (!strcmp(a, "-d")) {
			enable = 0;
		} else if (!strcmp(a, "-i")) {
			invert = 1;
		} else if (!strcmp(a, "-n")) {
			invert = 0;
		} else if (!strcmp(a, "--send-delay")) {
			dsend = strtoul(argv[++i], NULL, 0);
			hsend = 1;
		} else if (!strcmp(a, "--after-delay")) {
			dafter = strtoul(argv[++i], NULL, 0);
			hafter = 1;
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n",
				argv[0], a);
			usage(argv[0]);
		}
	}

	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	if (ioctl(fd, TIOCGRS485, &rs) < 0) {
		fprintf(stderr, "TIOCGRS485 %s: %s\n"
			"(driver has no RS485 support?)\n",
			dev, strerror(errno));
		close(fd);
		return 1;
	}

	new = rs;
	if (enable == 1) {
		new.flags |= SER_RS485_ENABLED;
		change = 1;
	} else if (enable == 0) {
		new.flags &= ~(unsigned int)SER_RS485_ENABLED;
		change = 1;
	}
	if (invert == 0) { /* asserted while driving, released when listening */
		new.flags |= SER_RS485_RTS_ON_SEND;
		new.flags &= ~(unsigned int)SER_RS485_RTS_AFTER_SEND;
		change = 1;
	} else if (invert == 1) {
		new.flags &= ~(unsigned int)SER_RS485_RTS_ON_SEND;
		new.flags |= SER_RS485_RTS_AFTER_SEND;
		change = 1;
	}
	if (hsend) {
		new.delay_rts_before_send = dsend;
		change = 1;
	}
	if (hafter) {
		new.delay_rts_after_send = dafter;
		change = 1;
	}

	if (get_only) {
		print_cfg(dev, &rs);
		close(fd);
		return 0;
	}

	if (change && ioctl(fd, TIOCSRS485, &new) < 0) {
		fprintf(stderr, "TIOCSRS485 %s: %s\n",
			dev, strerror(errno));
		close(fd);
		return 1;
	}

	print_cfg(dev, change ? &new : &rs);
	if (!change)
		printf("(no changes given, settings unchanged)\n");

	close(fd);
	return 0;
}
