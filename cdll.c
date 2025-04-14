/**********************************************************************
 *
 * Filename:    cdll.c
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
#include <cdll.h>
/*
 * cdll circular double linked list infrastructure
 * linked list implementation,
 * can be used as a FIFO, FILO or ring buffer.
 */
void cdll_init(struct cdll *listhead)
{
    listhead->next = listhead;
    listhead->prev = listhead;
}
static void cdll_add_node(struct cdll *new,
                          struct cdll *prev,
                          struct cdll *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}
/*
 * list_add_head - add a new entry
 * newnode: new entry to be added
 * head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing filo stacks.
 */
void cdll_insert_node_head(struct cdll *newnode, struct cdll *head)
{
    /* stick new node into head. */
    cdll_add_node(newnode, head, head->next);
}
/*
 * list_add_tail - add a new entry
 * newnode: new entry to be added
 * head: list head to add it after
 *
 * Insert a new entry befor the specified head.
 * This is good for implementing fifo queues.
 */
void cdll_insert_node_tail(struct cdll *newnode, struct cdll *head)
{
    /* stick new node into tail. */
    cdll_add_node(newnode, head->prev, head);
}
void cdll_delete_node(struct cdll *list)
{
    /* remove node from list. */
    list->prev->next = list->next;
    list->next->prev = list->prev;
}