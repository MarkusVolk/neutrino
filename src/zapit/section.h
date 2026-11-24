/*
 * License: GPLv2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __zapit_section_h__
#define __zapit_section_h__

/* a whole section of 12 bit length; libdvbsi++ trusts the lengths inside, and
 * a descriptor may claim up to 255 bytes past the end of its loop */
#define SECTION_SIZE		4098
#define SECTION_PADDING		260

/* len bytes were read: is it all of the section */
static inline bool sectionComplete(const unsigned char *buffer, int len)
{
	return len >= 3 && len >= (((buffer[1] & 0x0F) << 8) | buffer[2]) + 3;
}

#endif
