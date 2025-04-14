/**********************************************************************
 *
 * Filename:    cdll.h
 *
 * Description: define a double linked list infrastructure.
 *
 * Notes:       This software should be portable to Posix compatible systems.
 *
 *
 * Copyright (c) 2018 by Steve Calfee.  This software is placed into
 * the public domain and may be used for any purpose.  However, this
 * notice must not be changed or removed and no warranty is either
 * expressed or implied by its publication or distribution.
 **********************************************************************/
#ifndef __CDLL_INFRA_H__
#define __CDLL_INFRA_H__

/*
 * cdll circular double linked list infrastructure
 * linked list implementation,
 * can be used as a FIFO, FILO or ring buffer.
 */
struct cdll {
    struct cdll *next;
    struct cdll *prev;
};
//note both iterators below use current ll ptr, it is a problem if node is
//deleted and memory is changed see defs below. It is a case of using memory
//that has been deleted, not always safe.
#define cdll_for_each(pos, head) \
        for (pos = (head)->next; pos != (head); pos = pos->next)

#define cdll_for_each_rev(pos, head) \
        for (pos = (head)->prev; pos != (head); pos = pos->prev)

#define cdll_empty(head) ((head) == (head)->next)

void cdll_init(struct cdll *listhead);
/*
 * cdll_insert_node_head - add a new entry
 * newnode: new entry to be added
 * head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing filo stacks.
 */
void cdll_insert_node_head(struct cdll *newnode, struct cdll *head);
/*
 * cdll_insert_node_tail - add a new entry
 * newnode: new entry to be added
 * head: list head to add it after
 *
 * Insert a new entry befor the specified head.
 * This is good for implementing fifo queues.
 */
void cdll_insert_node_tail(struct cdll *newnode, struct cdll *head);
void cdll_delete_node(struct cdll *list);

/**
 * cast_p_to_outer - cast a pointer to an outer, containing struct
 * @ptype:The type of pointer (ie member type)
 * @ptr:The pointer to the member
 * @mtype:The type of the outer, container
 * @mname:The name of the member in outer struct (ptr is pointing at it)
 *
 * This uses a portable (no GNU extension) method of casting
 * All types are checked. Trust the C optimizer
 */
#define cast_p_to_outer(ptype, ptr, mtype, mname) (\
    ((ptype) ptr == ptr) ? \
    (mtype *)( (char *)ptr - offsetof(mtype, mname) ) \
    : NULL \
)

#endif //__CDLL_INFRA_H__