 /********************************************************************\
* guid.h -- globally unique ID User API                            *
* Copyright (C) 2000 Dave Peticolas <peticola@cs.ucdavis.edu>      *
*                                                                  *
* This program is free software; you can redistribute it and/or    *
* modify it under the terms of the GNU General Public License as   *
* published by the Free Software Foundation; either version 2 of   *
* the License, or (at your option) any later version.              *
*                                                                  *
* This program is distributed in the hope that it will be useful,  *
* but WITHOUT ANY WARRANTY; without even the implied warranty of   *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
* GNU General Public License for more details.                     *
*                                                                  *
* You should have received a copy of the GNU General Public License*
* along with this program; if not, contact:                        *
*                                                                  *
* Free Software Foundation           Voice:  +1-617-542-5942       *
* 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
* Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
*                                                                  *
\********************************************************************/
 
#ifndef GUID_H
#define GUID_H
#ifdef _GUID
#undef _GUID
#endif
#include <stddef.h>
#include <glib-object.h>

#define GUID_DATA_SIZE  16
typedef union _GUID
{
 guchar data[GUID_DATA_SIZE];

 gint __align_me;            /* this just ensures that GUIDs are 32-bit
                                 * aligned on systems that need them to be. */
} GUID;
 
 
#define GUID_ENCODING_LENGTH 32
 
 
void guid_init (void);
 
void guid_init_with_salt (const void *salt, size_t salt_len);
 
void guid_init_only_salt (const void *salt, size_t salt_len);
 
void guid_shutdown (void);
 
void guid_new (GUID * guid);
 
GUID guid_new_return (void);
 
const GUID *guid_null (void);
 
GUID *guid_malloc (void);
 
/* Return a guid set to all zero's */
void guid_free (GUID * guid);

const gchar *guid_to_string (const GUID * guid);

gchar *guid_to_string_buff (const GUID * guid, gchar * buff);


gboolean string_to_GUID (const gchar * string, GUID * guid);

 
gboolean guid_equal (const GUID * guid_1, const GUID * guid_2);
gint guid_compare (const GUID * g1, const GUID * g2);

guint guid_hash_to_guint (gconstpointer ptr);
 
GHashTable *guid_hash_table_new (void);
#undef _GUID
//#include "../common/ceph-mingw-type.h"
/* @} */
/* @} */
#endif