/*--------------------------------------------------------------------*/
/*--- Callgrind                                                    ---*/
/*---                                                 ct_context.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Callgrind, a Valgrind tool for call tracing.

   Copyright (C) 2002-2017, Josef Weidendorfer (Josef.Weidendorfer@gmx.de)

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "global.h"


/*------------------------------------------------------------*/
/*--- Context operations                                   ---*/
/*------------------------------------------------------------*/

#define N_FNSTACK_INITIAL_ENTRIES 500
#define N_CXT_INITIAL_ENTRIES 2537

fn_stack LPG_(current_fn_stack);

void LPG_(init_fn_stack)(fn_stack* s)
{
  LPG_ASSERT(s != 0);

  s->size   = N_FNSTACK_INITIAL_ENTRIES;   
  s->bottom = (fn_node**) LPG_MALLOC("cl.context.ifs.1",
                                     s->size * sizeof(fn_node*));
  s->top    = s->bottom;
  s->bottom[0] = 0;
}

void LPG_(copy_current_fn_stack)(fn_stack* dst)
{
  LPG_ASSERT(dst != 0);

  dst->size   = LPG_(current_fn_stack).size;
  dst->bottom = LPG_(current_fn_stack).bottom;
  dst->top    = LPG_(current_fn_stack).top;
}

void LPG_(set_current_fn_stack)(fn_stack* s)
{
  LPG_ASSERT(s != 0);

  LPG_(current_fn_stack).size   = s->size;
  LPG_(current_fn_stack).bottom = s->bottom;
  LPG_(current_fn_stack).top    = s->top;
}

static cxt_hash cxts;

void LPG_(init_cxt_table)()
{
   Int i;
   
   cxts.size    = N_CXT_INITIAL_ENTRIES;
   cxts.entries = 0;
   cxts.table   = (Context**) LPG_MALLOC("cl.context.ict.1",
                                         cxts.size * sizeof(Context*));

   for (i = 0; i < cxts.size; i++)
     cxts.table[i] = 0;
}

/* double size of cxt table  */
static void resize_cxt_table(void)
{
    UInt i, new_size, conflicts1 = 0, conflicts2 = 0;
    Context **new_table, *curr, *next;
    UInt new_idx;

    new_size  = 2* cxts.size +3;
    new_table = (Context**) LPG_MALLOC("cl.context.rct.1",
                                       new_size * sizeof(Context*));

    for (i = 0; i < new_size; i++)
      new_table[i] = NULL;

    for (i = 0; i < cxts.size; i++) {
        if (cxts.table[i] == NULL) continue;

        curr = cxts.table[i];
        while (NULL != curr) {
            next = curr->next;

            new_idx = (UInt) (curr->hash % new_size);

            curr->next = new_table[new_idx];
            new_table[new_idx] = curr;
            if (curr->next) {
                conflicts1++;
                if (curr->next->next)
                    conflicts2++;
            }

            curr = next;
        }
    }

    VG_(free)(cxts.table);


    LPG_DEBUG(0, "Resize Context Hash: %u => %u (entries %u, conflicts %u/%u)\n",
             cxts.size, new_size,
             cxts.entries, conflicts1, conflicts2);

    cxts.size  = new_size;
    cxts.table = new_table;
    LPG_(stat).cxt_hash_resizes++;
}

__inline__
static UWord cxt_hash_val(fn_node** fn, UInt size)
{
    UWord hash = 0;
    UInt count = size;
    while(*fn != 0) {
        hash = (hash<<7) + (hash>>25) + (UWord)(*fn);
        fn--;
        count--;
        if (count==0) break;
    }
    return hash;
}

__inline__
static Bool is_cxt(UWord hash, fn_node** fn, Context* cxt)
{
    int count;
    fn_node** cxt_fn;

    if (hash != cxt->hash) return False;

    count = cxt->size;
    cxt_fn = &(cxt->fn[0]);
    while((*fn != 0) && (count>0)) {
        if (*cxt_fn != *fn) return False;
        fn--;
        cxt_fn++;
        count--;
    }
    return True;
}

/**
 * Allocate new Context structure
 */
static Context* new_cxt(fn_node** fn)
{
    Context* cxt;
    UInt idx, offset;
    UWord hash;
    int size;
    fn_node* top_fn;

    LPG_ASSERT(fn);
    top_fn = *fn;
    if (top_fn == 0) return 0;

    size = 1;

    /* check fill degree of context hash table and resize if needed (>80%) */
    cxts.entries++;
    if (10 * cxts.entries / cxts.size > 8)
        resize_cxt_table();

    cxt = (Context*) LPG_MALLOC("cl.context.nc.1",
                                sizeof(Context)+sizeof(fn_node*)*size);

    // hash value calculation similar to cxt_hash_val(), but additionally
    // copying function pointers in one run
    hash = 0;
    offset = 0;
    while(*fn != 0) {
        hash = (hash<<7) + (hash>>25) + (UWord)(*fn);
	cxt->fn[offset] = *fn;
        offset++;
        fn--;
        if (offset >= size) break;
    }
    if (offset < size) size = offset;

    cxt->size        = size;
    cxt->base_number = LPG_(stat).context_counter;
    cxt->hash        = hash;

    LPG_(stat).context_counter++;
    LPG_(stat).distinct_contexts++;

    /* insert into Context hash table */
    idx = (UInt) (hash % cxts.size);
    cxt->next = cxts.table[idx];
    cxts.table[idx] = cxt;

#if LPG_ENABLE_DEBUG
    LPG_DEBUGIF(3) {
      VG_(printf)("  new_cxt ox%p: ", cxt);
      LPG_(print_cxt)(12, cxt);
    }
#endif

    return cxt;
}

/* get the Context structure for current context */
Context* LPG_(get_cxt)(fn_node** fn)
{
    Context* cxt;
    UInt size, idx;
    UWord hash;

    LPG_ASSERT(fn != 0);
    if (*fn == 0) return 0;
    size = 1;
    if (size<=0) { size = -size+1; }

    LPG_DEBUG(5, "+ get_cxt(fn '%s'): size %u\n",
                (*fn)->name, size);

    hash = cxt_hash_val(fn, size);

    if ( ((cxt = (*fn)->last_cxt) != 0) && is_cxt(hash, fn, cxt)) {
        LPG_DEBUG(5, "- get_cxt: %p\n", cxt);
        return cxt;
    }

    LPG_(stat).cxt_lru_misses++;

    idx = (UInt) (hash % cxts.size);
    cxt = cxts.table[idx];

    while(cxt) {
        if (is_cxt(hash,fn,cxt)) break;
        cxt = cxt->next;
    }

    if (!cxt)
        cxt = new_cxt(fn);

    (*fn)->last_cxt = cxt;

    LPG_DEBUG(5, "- get_cxt: %p\n", cxt);

    return cxt;
}


/**
 * Change execution context by calling a new function from current context
 * Pushing 0x0 specifies a marker for a signal handler entry
 */
void LPG_(push_cxt)(fn_node* fn)
{
  call_stack* cs = &LPG_(current_call_stack);
  Int fn_entries;

  LPG_DEBUG(5, "+ push_cxt(fn '%s'): old ctx %d\n", 
	    fn ? fn->name : "0x0",
	    LPG_(current_state).cxt ?
	    (Int)LPG_(current_state).cxt->base_number : -1);

  /* save old context on stack (even if not changed at all!) */
  LPG_ASSERT(cs->sp < cs->size);
  LPG_ASSERT(cs->entry[cs->sp].cxt == 0);
  cs->entry[cs->sp].cxt = LPG_(current_state).cxt;
  cs->entry[cs->sp].fn_sp = LPG_(current_fn_stack).top - LPG_(current_fn_stack).bottom;

  if (fn && (*(LPG_(current_fn_stack).top) == fn)) return;

  /* resizing needed ? */
  fn_entries = LPG_(current_fn_stack).top - LPG_(current_fn_stack).bottom;
  if (fn_entries == LPG_(current_fn_stack).size-1) {
    UInt new_size = LPG_(current_fn_stack).size *2;
    fn_node** new_array = (fn_node**) LPG_MALLOC("cl.context.pc.1",
						 new_size * sizeof(fn_node*));
    int i;
    for(i=0;i<LPG_(current_fn_stack).size;i++)
      new_array[i] = LPG_(current_fn_stack).bottom[i];
    VG_(free)(LPG_(current_fn_stack).bottom);
    LPG_(current_fn_stack).top = new_array + fn_entries;
    LPG_(current_fn_stack).bottom = new_array;

    LPG_DEBUG(0, "Resize Context Stack: %u => %u (pushing '%s')\n", 
	     LPG_(current_fn_stack).size, new_size,
	     fn ? fn->name : "0x0");

    LPG_(current_fn_stack).size = new_size;
  }

  if (fn && (*(LPG_(current_fn_stack).top) == 0)) {
    UInt *pactive;

    /* this is first function: increment its active count */
    pactive = LPG_(get_fn_entry)(fn->number);
    (*pactive)++;
  }

  LPG_(current_fn_stack).top++;
  *(LPG_(current_fn_stack).top) = fn;
  LPG_(current_state).cxt = LPG_(get_cxt)(LPG_(current_fn_stack).top);

  LPG_DEBUG(5, "- push_cxt(fn '%s'): new cxt %d, fn_sp %ld\n",
	    fn ? fn->name : "0x0",
	    LPG_(current_state).cxt ?
	    (Int)LPG_(current_state).cxt->base_number : -1,
	    LPG_(current_fn_stack).top - LPG_(current_fn_stack).bottom + 0L);
}
			       
