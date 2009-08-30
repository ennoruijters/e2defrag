/*
    Copyright 2009 Enno Ruijters

    This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General
    Public License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include "e2defrag.h"
#include "extree.h"

void dump_trees(struct defrag_ctx *c, int to_dump)
{
	struct rb_node *n = rb_first(&c->free_tree_by_block);
	while (n && (to_dump & 1)) {
		struct free_extent *f = rb_entry(n, struct free_extent,block_rb);
		printf("F: %llu-%llu (%llu)\n", f->start_block, f->end_block,
		       f->end_block - f->start_block + 1);
		n = rb_next(n);
	}
	n = rb_first(&c->extents_by_block);
	while (n && (to_dump & 2)) {
		struct data_extent *f = rb_entry(n,struct data_extent,block_rb);
		printf("U: %llu-%llu(%llu)%s of %u\n", f->start_block,
		       f->end_block, f->start_logical,
		       f->uninit ? "U" : "", f->inode_nr);
		n = rb_next(n);
	}
}
