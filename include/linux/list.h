/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/poison.h>
#include <linux/const.h>
#include <linux/kernel.h>

/*
 * 간단 이중 링크드 리스트 구현
 *
 * 일부 내부 함수("___xxx")는 단일 항목보다 전체 리스트를 조작할 때 유용하다.
 * next/prev 항목을 이미 알고 있들때 일반 단일 항목 루틴을 사용하는 것보다
 * 그것들(내부함수)을 직접 사용하여 더 나은 코드를 생성 할 수 있기 때문이다.
 */
#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	list->prev = list;
}

#ifdef CONFIG_DEBUG_LIST
extern bool __list_add_valid(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next);
extern bool __list_del_entry_valid(struct list_head *entry);
#else
static inline bool __list_add_valid(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	return true;
}
static inline bool __list_del_entry_valid(struct list_head *entry)
{
	return true;
}
#endif

/*
 * 두 알려진 연속 항목 사이에 새로운 항목을 삽입하라.
 *
 * prev/next 항목을 이미 알고 있는 내부 리스트 조작만을 위한 것이다 .
 */
/* prev와 next 사이에 new를 삽입한다. prev - new -next 순 */
static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	if (!__list_add_valid(new, prev, next))
		return;

	next->prev = new;
	new->next = next;
	new->prev = prev;
	WRITE_ONCE(prev->next, new);
}

/**
 * list_add - add a new entry
 * @new: new entry to be added
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
/* head의 처음(head와 head->next 사이)에 new 삽입. head - new - head->next 순 */
static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}


/**
 * list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
/* head의 끝(head->prev와 head 사이)에 new 삽입. head->prev - new - head 순 */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
/*
 * prev/next가 각각을 가리키게 함으로써 리스트 항목을 삭제한다.
 *
 * prev/next 항목을 이미 알때 사용하는 내부 리스트 조작을 위한 것이다.
 */
/* prev - next 를 연결한다. 원래는 prev - entry - next 순. */
static inline void __list_del(struct list_head * prev, struct list_head * next)
{
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
}

/**
 * list_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: list_empty() on entry does not return true after this, the entry is
 * in an undefined state.
 */
/*
 * entry->prev - entry->next를 연결하는 방식으로 entry 삭제.
 * CONFIG_DEBUG_LIST 설정시 유효성 검사
 */
static inline void __list_del_entry(struct list_head *entry)
{
	if (!__list_del_entry_valid(entry))
		return;

	__list_del(entry->prev, entry->next);
}

/* entry->prev - entry->next 를 연결하고 entry의 멤버들은 LIST_POISON 값 설정 */
static inline void list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

/**
 * list_replace - replace old entry by new one
 * @old : the element to be replaced
 * @new : the new element to insert
 *
 * If @old was empty, it will be overwritten.
 */
/* prev - old - next 에서 old가 new로 교체되어 prev - new - next 순 */
static inline void list_replace(struct list_head *old,
				struct list_head *new)
{
	new->next = old->next;
	new->next->prev = new;
	new->prev = old->prev;
	new->prev->next = new;
}

/* old를 new로 교체하고 old는 초기화 */
static inline void list_replace_init(struct list_head *old,
					struct list_head *new)
{
	list_replace(old, new);
	INIT_LIST_HEAD(old);
}

/**
 * list_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
/* list_del과 다른점은 삭제한 entry 에 대하여 초기화 */
static inline void list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

/**
 * list_move - 한 목록에서 삭제하고 다른 목록에 더함
 * @list: 옮길 항목
 * @head: 항목 앞에 위치할 head
 */
/* prev - list - next 에서 list 를 삭제하여 head - head->next 사이로 옮김 */
static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add(list, head);
}

/**
 * list_move_tail - 한 목록에서 삭제하고 다른 목록의 꼬리에 더함
 * @list: 옮길 항목
 * @head: 항목 에 따라올 head
 */
/* prev - list - next 에서 list 를 삭제하여 head->prev - head 사이로 옮김 */
static inline void list_move_tail(struct list_head *list,
				  struct list_head *head)
{
	__list_del_entry(list);
	list_add_tail(list, head);
}

/**
 * list_bulk_move_tail - 리스트의 일부를 꼬리로 옮김
 * @head: the head that will follow our entry
 * @first: first entry to move
 * @last: last entry to move, can be the same as first
 *
 * @first와 @last포함하는 사이의 항목을 @head 이전으로 옮겨라.
 * 세 항목 모두 같은 linked 리스트에 속해야 한다.
 */
/* head->prev - first - ... - last - head 순이 된다 */
static inline void list_bulk_move_tail(struct list_head *head,
				       struct list_head *first,
				       struct list_head *last)
{
	first->prev->next = last->next;
	last->next->prev = first->prev;

	head->prev->next = first;
	first->prev = head->prev;

	last->next = head;
	head->prev = last;
}

/**
 * list_is_first -- @list가 @head 목록에서 첫번째 항목인지 테스트
 * @list: 테스트할 항목
 * @head: 목록의 head
 */
static inline int list_is_first(const struct list_head *list,
					const struct list_head *head)
{
	return list->prev == head;
}

/**
 * list_is_last - 리스트가 목록의 마지막 항목인지 테스트
 * @list: 테스트할 항목
 * @head: 리스트의 head
 */
static inline int list_is_last(const struct list_head *list,
				const struct list_head *head)
{
	return list->next == head;
}

/**
 * list_empty - 리스트가 비었는지 테스트
 * @head: 테스트할 리스트
 */
static inline int list_empty(const struct list_head *head)
{
	return READ_ONCE(head->next) == head;
}

/*
 * list_empty_careful - 리스트가 비어있고 변경되지 않는지 테스트
 * @head: 테스트할 목록
 *
 * 설명:
 * 리스트가 비어있고 멤버(next 나 prev)를 변경하는 프로세스가 없는지를 확인
 *
 * 참고: 동기화 없는 list_empty_careful()의 사용은 리스트 항목에 일어날 수 있는
 * 것이 list_del_init()만 있을 때 안전하다. 예. 다른 CPU가 다시 list_add()를
 * 사용할 수 있다면 사용할 수 없다.
 */
static inline int list_empty_careful(const struct list_head *head)
{
	struct list_head *next = head->next;
	return (next == head) && (next == head->prev);
}

/**
 * list_rotate_left - 왼쪽으로 목록 회전
 * @head: 리스트의 head
 */
/* 목록의 첫번째 항목을 꼬리로 옮겨서 이중원형리스트를 시계반대방향으로 회전 */
static inline void list_rotate_left(struct list_head *head)
{
	struct list_head *first;

	if (!list_empty(head)) {
		first = head->next;
		list_move_tail(first, head);
	}
}


/**
 * list_is_singular - 목록에 항목 하나만 있는지 테스트
 * @head: 테스트할 목록
 */
/* 이중원형리스트에서 head->next와 head->prev 가 같다는 것은 목록이 하나 */
static inline int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && (head->next == head->prev);
}

/* head->next에서 entry 까지 잘라 리트스에 복사. list = head->next - ... - entry */
/* 기존 head는 entry 까지 삭게되고 entry 다음 부터 계속됨. head - entry->next - ... */
static inline void __list_cut_position(struct list_head *list,
		struct list_head *head, struct list_head *entry)
{
	struct list_head *new_first = entry->next;
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

/*
 * list_cut_position - list를 둘로 잘라낸다
 * @list: 모든 옮겨질 항목을 더할 새 리스트
 * @head: 항목이 있는 리스트
 * @entry: head에 속한 항목. 리스트를 자르기 원치 않느다면 head 자신이 될 수 있다.
 *
 * 이 도우미 함수는 @head의 처음부터 @entry(포함)까지 @head에서 @list로 옯긴다.
 * @head 에 있는 요소을 @entry에 전달 해야한다. @list는 빈 리스트 이거나
 * data를 손실에 신경쓰지 않는 리스트여야 한다.
 *
 */
static inline void list_cut_position(struct list_head *list,
		struct list_head *head, struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (list_is_singular(head) &&
		(head->next != entry && head != entry))
		return;
	if (entry == head)
		INIT_LIST_HEAD(list);
	else
		__list_cut_position(list, head, entry);
}

/**
 * list_cut_before - cut a list into two, before given entry
 * @list: a new list to add all removed entries
 * @head: a list with entries
 * @entry: an entry within head, could be the head itself
 *
 * This helper moves the initial part of @head, up to but
 * excluding @entry, from @head to @list.  You should pass
 * in @entry an element you know is on @head.  @list should
 * be an empty list or a list you do not care about losing
 * its data.
 * If @entry == @head, all entries on @head are moved to
 * @list.
 */
/* list_cut_position 과 같으나 entry 전까지 잘라낸다 */
/* list = head->next - ... - entry->prev */
static inline void list_cut_before(struct list_head *list,
				   struct list_head *head,
				   struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}

/*
 * prev와 next사이에 list를 연결한다.
 * prev - list->next - ... - list->prev - next 순
 */
static inline void __list_splice(const struct list_head *list,
				 struct list_head *prev,
				 struct list_head *next)
{
	struct list_head *first = list->next;
	struct list_head *last = list->prev;

	first->prev = prev;
	prev->next = first;

	last->next = next;
	next->prev = last;
}

/**
 * list_splice - join two lists, this is designed for stacks
 * @list: the new list to add.
 * @head: the place to add it in the first list.
 */
/* head의 처음(head와 head->next 사이)에 list를 붙인다. */
/* head - list->next - ... - list->prev(last) - head->next - ... */
static inline void list_splice(const struct list_head *list,
				struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}

/**
 * list_splice_tail - join two lists, each list being a queue
 * @list: the new list to add.
 * @head: the place to add it in the first list.
 */
/* head의 끝(head->prev와 head 사이)에 list를 붙인다. */
/* head->prev - list->next - ... - list->prev(last) - head - ... */
static inline void list_splice_tail(struct list_head *list,
				struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head->prev, head);
}

/**
 * list_splice_init - join two lists and reinitialise the emptied list.
 * @list: the new list to add.
 * @head: the place to add it in the first list.
 *
 * The list at @list is reinitialised
 */
/*
 * list_splice_init - 두 리스트를 연결하고 비어있는 목록을 다시 초기화 한다
 * @list: 추가할 새 리스트
 * @head: 첫번째 리스트를 더 할 곳
 *
 * @list는 다시 초기화 된다
 */
/* @list를 @head의 처음에 연결후 @list는 기존 연결이 깨어지므로 다시 초기화 한다 */
static inline void list_splice_init(struct list_head *list,
				    struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}

/**
 * list_splice_tail_init - join two lists and reinitialise the emptied list
 * @list: the new list to add.
 * @head: the place to add it in the first list.
 *
 * Each of the lists is a queue.
 * The list at @list is reinitialised
 */
/* @list를 @head의 끝에 연결후 @list는 기존 연결이 깨어지므로 다시 초기화 한다 */
static inline void list_splice_tail_init(struct list_head *list,
					 struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
/*
 * @ptr이 속한 구조체의 포인터. 매크로명에 entry가 있으면 container_of 를 사용하여
 * @ptr에서 member 오프셋만큼 빼기하여 @ptr이 속한 구조체의 포인터를 알아온다.
 */
#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

/*
 * 커널 list api에서 함수명에 entry가 있으면 get the element 로
 * 주석이 작성되는데 여기서 element 는 container_of 함수로 가져오게 되는
 * @ptr이 속한 구조체의 포인터이다. 예. page 구조체(type=page)에 멤버변수명이
 * lru(member=lru)이고 이 멤버변수의 변수명이 plru(ptr=plru)인 경우
 * 이 plru의 주소에서 페이지 구조체의 lru 멤버변수의 offset 위치 만큼 빼서
 * 페이지 구조체 포인터를 얻는다.
 */
/**
 * list_first_entry - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

/**
 * list_last_entry - get the last element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

/**
 * list_first_entry_or_null - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note that if the list is empty, it returns NULL.
 */
/*
 * 목록의 첫번째 요소를 가져옮. 목록이 비었으면 NULL 반환
 * pos__(head__->next)와 head 가 같지 않다는 건 목록의 첫번째 항목이
 * 존재한다는 것이다. 단 매개변수로 온 ptr은 반드시 목록이 head 이어야 한다.
 */
#define list_first_entry_or_null(ptr, type, member) ({ \
	struct list_head *head__ = (ptr); \
	struct list_head *pos__ = READ_ONCE(head__->next); \
	pos__ != head__ ? list_entry(pos__, type, member) : NULL; \
})

/**
 * list_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * list_prev_entry - get the prev element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * list_for_each	-	iterate over a list
 * @pos:	the &struct list_head to use as a loop cursor.
 * @head:	the head for your list.
 */
/* 처음(head->next)에서 끝(head->prev) 까지 반복 */
#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * list_for_each_prev	-	iterate over a list backwards
 * @pos:	the &struct list_head to use as a loop cursor.
 * @head:	the head for your list.
 */
/* 끝(head->prev)에서 처음(head->next)까지 역순으로 반복 */
#define list_for_each_prev(pos, head) \
	for (pos = (head)->prev; pos != (head); pos = pos->prev)

/**
 * list_for_each_safe - iterate over a list safe against removal of list entry
 * @pos:	the &struct list_head to use as a loop cursor.
 * @n:		another &struct list_head to use as temporary storage
 * @head:	the head for your list.
 */
/*
 * FIXME - list_for_each와 다른 점은 pos->next 값을 미리 구해서 저장해 놓는 것인데
 * 삭제에 안전하다는 정확한 의미를 모르겠다
 */
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

/**
 * list_for_each_prev_safe - iterate over a list backwards safe against removal of list entry
 * @pos:	the &struct list_head to use as a loop cursor.
 * @n:		another &struct list_head to use as temporary storage
 * @head:	the head for your list.
 */
#define list_for_each_prev_safe(pos, n, head) \
	for (pos = (head)->prev, n = pos->prev; \
	     pos != (head); \
	     pos = n, n = pos->prev)

/**
 * list_for_each_entry	-	iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
/*
 * 처음 요소(container_of(head->next, ...))에서
 * 끝까지(container_of(head->prev,...)) 반복
 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

/**
 * list_for_each_entry_reverse - iterate backwards over list of given type.
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
/*
 * 끝 요소(container_of(head->prev, ...))에서
 * 처음(container_of(head->next, ...))까지  역순으로 반복
 */
#define list_for_each_entry_reverse(pos, head, member)			\
	for (pos = list_last_entry(head, typeof(*pos), member);		\
	     &pos->member != (head); 					\
	     pos = list_prev_entry(pos, member))

/**
 * list_prepare_entry - prepare a pos entry for use in list_for_each_entry_continue()
 * @pos:	the type * to use as a start point
 * @head:	the head of the list
 * @member:	the name of the list_head within the struct.
 *
 * Prepares a pos entry for use as a start point in list_for_each_entry_continue().
 */
/* pos가 NULL이 아니면 pos 를 반환하고 NULL 이면 container_of(head,...) 요소를 반환 */
#define list_prepare_entry(pos, head, member) \
	((pos) ? : list_entry(head, typeof(*pos), member))

/**
 * list_for_each_entry_continue - continue iteration over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Continue to iterate over list of given type, continuing after
 * the current position.
 */
/*
 * 현재(pos)의 다음(container_of(pos->member.next, ...)) 요소 에서
 * 끝(container_of(head->prev,...))까지 반복
 */
#define list_for_each_entry_continue(pos, head, member) 		\
	for (pos = list_next_entry(pos, member);			\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

/**
 * list_for_each_entry_continue_reverse - iterate backwards from the given point
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Start to iterate over list of given type backwards, continuing after
 * the current position.
 */
/*
 * 현재(pos)의 이전(container_of(pos->member.prev, ...)) 요소 에서
 * 처음(container_of(head->prev,...))까지 역으로 반복
 */
#define list_for_each_entry_continue_reverse(pos, head, member)		\
	for (pos = list_prev_entry(pos, member);			\
	     &pos->member != (head);					\
	     pos = list_prev_entry(pos, member))

/**
 * list_for_each_entry_from - iterate over list of given type from the current point
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type, continuing from current position.
 */
/* pos(포함)부터 끝까지 entry 반복 */
#define list_for_each_entry_from(pos, head, member) 			\
	for (; &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

/**
 * list_for_each_entry_from_reverse - iterate backwards over list of given type
 *                                    from the current point
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate backwards over list of given type, continuing from current position.
 */
/* pos(포함)부터 처음까지 entry 역방향으로 반복 */
#define list_for_each_entry_from_reverse(pos, head, member)		\
	for (; &pos->member != (head);					\
	     pos = list_prev_entry(pos, member))

/**
 * list_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_first_entry(head, typeof(*pos), member),	\
		n = list_next_entry(pos, member);			\
	     &pos->member != (head); 					\
	     pos = n, n = list_next_entry(n, member))

/**
 * list_for_each_entry_safe_continue - continue list iteration safe against removal
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type, continuing after current point,
 * safe against removal of list entry.
 */
#define list_for_each_entry_safe_continue(pos, n, head, member) 		\
	for (pos = list_next_entry(pos, member), 				\
		n = list_next_entry(pos, member);				\
	     &pos->member != (head);						\
	     pos = n, n = list_next_entry(n, member))

/**
 * list_for_each_entry_safe_from - iterate over list from current point safe against removal
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type from current point, safe against
 * removal of list entry.
 */
#define list_for_each_entry_safe_from(pos, n, head, member) 			\
	for (n = list_next_entry(pos, member);					\
	     &pos->member != (head);						\
	     pos = n, n = list_next_entry(n, member))

/**
 * list_for_each_entry_safe_reverse - iterate backwards over list safe against removal
 * @pos:	the type * to use as a loop cursor.
 * @n:		another type * to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate backwards over list of given type, safe against removal
 * of list entry.
 */
#define list_for_each_entry_safe_reverse(pos, n, head, member)		\
	for (pos = list_last_entry(head, typeof(*pos), member),		\
		n = list_prev_entry(pos, member);			\
	     &pos->member != (head); 					\
	     pos = n, n = list_prev_entry(n, member))

/**
 * list_safe_reset_next - reset a stale list_for_each_entry_safe loop
 * @pos:	the loop cursor used in the list_for_each_entry_safe loop
 * @n:		temporary storage used in list_for_each_entry_safe
 * @member:	the name of the list_head within the struct.
 *
 * list_safe_reset_next is not safe to use in general if the list may be
 * modified concurrently (eg. the lock is dropped in the loop body). An
 * exception to this is if the cursor element (pos) is pinned in the list,
 * and list_safe_reset_next is called after re-taking the lock and before
 * completing the current iteration of the loop body.
 */
#define list_safe_reset_next(pos, n, member)				\
	n = list_next_entry(pos, member)

/*
 * Double linked lists with a single pointer list head.
 * Mostly useful for hash tables where the two pointer list head is
 * too wasteful.
 * You lose the ability to access the tail in O(1).
 */

#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = {  .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

static inline int hlist_unhashed(const struct hlist_node *h)
{
	return !h->pprev;
}

static inline int hlist_empty(const struct hlist_head *h)
{
	return !READ_ONCE(h->first);
}

static inline void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;

	WRITE_ONCE(*pprev, next);
	if (next)
		next->pprev = pprev;
}

static inline void hlist_del(struct hlist_node *n)
{
	__hlist_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

static inline void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	WRITE_ONCE(h->first, n);
	n->pprev = &h->first;
}

/* next must be != NULL */
static inline void hlist_add_before(struct hlist_node *n,
					struct hlist_node *next)
{
	n->pprev = next->pprev;
	n->next = next;
	next->pprev = &n->next;
	WRITE_ONCE(*(n->pprev), n);
}

static inline void hlist_add_behind(struct hlist_node *n,
				    struct hlist_node *prev)
{
	n->next = prev->next;
	WRITE_ONCE(prev->next, n);
	n->pprev = &prev->next;

	if (n->next)
		n->next->pprev  = &n->next;
}

/* after that we'll appear to be on some hlist and hlist_del will work */
static inline void hlist_add_fake(struct hlist_node *n)
{
	n->pprev = &n->next;
}

static inline bool hlist_fake(struct hlist_node *h)
{
	return h->pprev == &h->next;
}

/*
 * Check whether the node is the only node of the head without
 * accessing head:
 */
static inline bool
hlist_is_singular_node(struct hlist_node *n, struct hlist_head *h)
{
	return !n->next && n->pprev == &h->first;
}

/*
 * Move a list from one list head to another. Fixup the pprev
 * reference of the first entry if it exists.
 */
static inline void hlist_move_list(struct hlist_head *old,
				   struct hlist_head *new)
{
	new->first = old->first;
	if (new->first)
		new->first->pprev = &new->first;
	old->first = NULL;
}

#define hlist_entry(ptr, type, member) container_of(ptr,type,member)

#define hlist_for_each(pos, head) \
	for (pos = (head)->first; pos ; pos = pos->next)

#define hlist_for_each_safe(pos, n, head) \
	for (pos = (head)->first; pos && ({ n = pos->next; 1; }); \
	     pos = n)

#define hlist_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
	})

/**
 * hlist_for_each_entry	- iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry(pos, head, member)				\
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member);\
	     pos;							\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

/**
 * hlist_for_each_entry_continue - iterate over a hlist continuing after current point
 * @pos:	the type * to use as a loop cursor.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry_continue(pos, member)			\
	for (pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member);\
	     pos;							\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

/**
 * hlist_for_each_entry_from - iterate over a hlist continuing from current point
 * @pos:	the type * to use as a loop cursor.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry_from(pos, member)				\
	for (; pos;							\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

/**
 * hlist_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @pos:	the type * to use as a loop cursor.
 * @n:		another &struct hlist_node to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry_safe(pos, n, head, member) 		\
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member);\
	     pos && ({ n = pos->member.next; 1; });			\
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#endif
