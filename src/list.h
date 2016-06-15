/*
 * A simple realization of double list, refer to linux kernel.
 * Author: Chong Zhao
 * E-mail: zhaochong@pku.edu.cn
 */

#ifndef __SIMPLE_DOUBLE_LIST__
#define __SIMPLE_DOUBLE_LIST__

#include <stdio.h>

# define POISON_POINTER_DELTA 0

#define LIST_POISON1  ((void *) 0x00000100 + POISON_POINTER_DELTA)
#define LIST_POISON2  ((void *) 0x00000200 + POISON_POINTER_DELTA)

#define offsetof(type, member) (size_t)(&((type *)0)->member)

#define container_of(ptr, type, member) ({	\
	const typeof(((type *)0)->member)*__mptr = (ptr);	\
	(type *)((char *)__mptr - offsetof(type, member)); })

/* 第一个节点不存储数据 */
struct _list_head {
	struct _list_head *prev;
	struct _list_head *next;
};
typedef struct _list_head list_head;

static inline void init_list_head(list_head *list)
{
	list->prev = list;
	list->next = list;
}

static inline void __list_add(list_head *new_node,
		list_head *prev, list_head *next)
{
	prev->next = new_node;
	new_node->prev = prev;
	new_node->next = next;
	next->prev = new_node;
}

/* 从头部添加 */
static inline void list_add(list_head *new_node, list_head *head)
{
    __list_add(new_node, head, head->next);
}

/* 从尾部添加 */
static inline void list_add_tail(list_head *new_node, list_head *head)
{
    __list_add(new_node, head->prev, head);
}

static inline  void __list_del(list_head *prev, list_head *next)
{
    prev->next = next;
    next->prev = prev;
}

static inline void list_del(list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = LIST_POISON1;
    entry->prev = LIST_POISON2;
}

/* 移到头部 */
static inline void list_move(list_head *list, list_head *head)
{
	__list_del(list->prev, list->next);
	list_add(list, head);
}

/* 移动到尾部 */
static inline void list_move_tail(list_head *list, list_head *head)
{
	__list_del(list->prev, list->next);
	list_add_tail(list, head);
}
#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)


#endif /* end of __SIMPLE_DOUBLE_LIST__ */
