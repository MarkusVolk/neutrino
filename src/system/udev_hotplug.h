/*
	udev hotplug: block devices that come and go, passed on as EVT_HOTPLUG

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __system_udev_hotplug__
#define __system_udev_hotplug__

/*
 * With --enable-udev nobody runs mdev_helper. This thread follows the
 * block devices through libudev and sends the same EVT_HOTPLUG for every
 * partition, or disk with a filesystem of its own, that is added or removed.
 */
namespace udev_hotplug
{
	void start();
}

#endif
