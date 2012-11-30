/**
 * kalu - Copyright (C) 2012 Olivier Brunel
 *
 * shared.h
 * Copyright (C) 2012 Olivier Brunel <i.am.jack.mail@gmail.com>
 *
 * This file is part of kalu.
 *
 * kalu is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * kalu is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * kalu. If not, see http://www.gnu.org/licenses/
 */

#ifndef _SHARED_H
#define _SHARED_H

/* C */
#include <stdlib.h>
#include <string.h>

#define _UNUSED_                __attribute__ ((unused))

#define streq(s1, s2)           (strcmp (s1, s2) == 0)
#define memzero(x, l)           (memset (x, 0, l))
#define zero(x)                 (memzero (&(x), sizeof (x)))

#define new(type, len)          \
    (type *) _alloc (sizeof (type) * (size_t) (len), 0)
#define new0(type, len)         \
    (type *) _alloc (sizeof (type) * (size_t) (len), 1)
#define renew(type, len, ptr)   \
    (type *) _realloc (ptr, sizeof (type) * (size_t) (len))

#define FOR_LIST(i, val)        \
    for (i = (alpm_list_t *) (val); i; i = i->next)

void *_alloc (size_t len, int zero);
void *_realloc (void *ptr, size_t len);

#endif /* _SHARED_H */