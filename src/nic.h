 /*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 */

#ifndef	NIC_H
#define NIC_H

#include "dev.h"

/*
 *	Structure returned from eth_probe and passed to other driver
 *	functions.
 */
struct nic
{
	struct dev	dev;  /* This must come first */
	int		(*poll)P((struct nic *));
	void		(*transmit)P((struct nic *, const char *d,
				unsigned int t, unsigned int s, const char *p));
	int		flags;	/* driver specific flags */
	struct rom_info	*rom_info;	/* -> rom_info from main */
	unsigned char	*node_addr;
	unsigned char	*packet;
	unsigned int	packetlen;
	void		*priv_data;	/* driver can hang private data here */
};

#endif	/* NIC_H */
