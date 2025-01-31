/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_queue.h"

#ifdef CONFIG_USERSPACE

#define STACK_SIZE (512 + CONFIG_TEST_EXTRA_STACK_SIZE)
#define LIST_LEN        5

static K_THREAD_STACK_DEFINE(child_stack, STACK_SIZE);
static struct k_thread child_thread;
static ZTEST_BMEM struct qdata qdata[LIST_LEN * 2];

/**
 * @brief Tests for queue
 * @defgroup kernel_queue_tests Queues
 * @ingroup all_tests
 * @{
 * @}
 */

/* Higher priority than the thread putting stuff in the queue */
void child_thread_get(void *p1, void *p2, void *p3)
{
	struct qdata *qd;
	struct k_queue *q = p1;
	struct k_sem *sem = p2;

	zassert_false(k_queue_is_empty(q), NULL);
	qd = k_queue_peek_head(q);
	zassert_equal(qd->data, 0, NULL);
	qd = k_queue_peek_tail(q);
	zassert_equal(qd->data, (LIST_LEN * 2) - 1,
		      "got %d expected %d", qd->data, (LIST_LEN * 2) - 1);

	for (int i = 0; i < (LIST_LEN * 2); i++) {
		qd = k_queue_get(q, K_FOREVER);

		zassert_equal(qd->data, i, NULL);
		if (qd->allocated) {
			/* snode should never have been touched */
			zassert_is_null(qd->snode.next, NULL);
		}
	}


	zassert_true(k_queue_is_empty(q), NULL);

	/* This one gets canceled */
	qd = k_queue_get(q, K_FOREVER);
	zassert_is_null(qd, NULL);

	k_sem_give(sem);
}

/**
 * @brief Verify queue elements and cancel wait from a user thread
 *
 * @details The test adds elements to queue and then
 * verified by the child user thread.
 * Get data from a empty queue,and use K_FORVER to wait for available
 * And to cancel wait from current thread.
 *
 * @ingroup kernel_queue_tests
 *
 * @see k_queue_append(), k_queue_alloc_append(),
 * k_queue_init(), k_queue_cancel_wait()
 */
ZTEST(queue_api_1cpu, test_queue_supv_to_user)
{
	/* Supervisor mode will add a bunch of data, some with alloc
	 * and some not
	 */

	struct k_queue *q;
	struct k_sem *sem;

	if (!(IS_ENABLED(CONFIG_USERSPACE))) {
		ztest_test_skip();
	}

	q = k_object_alloc(K_OBJ_QUEUE);
	zassert_not_null(q, "no memory for allocated queue object");
	k_queue_init(q);

	sem = k_object_alloc(K_OBJ_SEM);
	zassert_not_null(sem, "no memory for semaphore object");
	k_sem_init(sem, 0, 1);

	for (int i = 0; i < (LIST_LEN * 2); i = i + 2) {
		/* Just for test purposes -- not safe to do this in the
		 * real world as user mode shouldn't have any access to the
		 * snode struct
		 */
		qdata[i].data = i;
		qdata[i].allocated = false;
		qdata[i].snode.next = NULL;
		k_queue_append(q, &qdata[i]);

		qdata[i + 1].data = i + 1;
		qdata[i + 1].allocated = true;
		qdata[i + 1].snode.next = NULL;
		zassert_false(k_queue_alloc_append(q, &qdata[i + 1]), NULL);
	}

	k_thread_create(&child_thread, child_stack, STACK_SIZE,
			child_thread_get, q, sem, NULL, K_HIGHEST_THREAD_PRIO,
			K_USER | K_INHERIT_PERMS, K_NO_WAIT);

	k_yield();

	/* child thread runs until blocking on the last k_queue_get() call */
	k_queue_cancel_wait(q);
	k_sem_take(sem, K_FOREVER);
}

/**
 * @brief verify allocate and feature "Last In, First Out"
 *
 * @details Create a new queue
 * And allocated memory for the queue
 * Initialize and insert data item in sequence.
 * Verify the feather "Last in,First out"
 *
 * @ingroup kernel_queue_tests
 *
 * @see k_queue_alloc_prepend()
 */
ZTEST_USER(queue_api, test_queue_alloc_prepend_user)
{
	struct k_queue *q;

	q = k_object_alloc(K_OBJ_QUEUE);
	zassert_not_null(q, "no memory for allocated queue object");
	k_queue_init(q);

	for (int i = 0; i < LIST_LEN * 2; i++) {
		qdata[i].data = i;
		zassert_false(k_queue_alloc_prepend(q, &qdata[i]), NULL);
	}

	for (int i = (LIST_LEN * 2) - 1; i >= 0; i--) {
		struct qdata *qd;

		qd = k_queue_get(q, K_NO_WAIT);
		zassert_true(qd != NULL, NULL);
		zassert_equal(qd->data, i, NULL);
	}
}

/**
 * @brief verify feature of queue "First In, First Out"
 *
 * @details Create a new queue
 * And allocated memory for the queue
 * Initialize and insert data item in sequence.
 * Verify the feather "First in,First out"
 *
 * @ingroup kernel_queue_tests
 *
 * @see k_queue_init(), k_queue_alloc_append()
 */
ZTEST_USER(queue_api, test_queue_alloc_append_user)
{
	struct k_queue *q;

	q = k_object_alloc(K_OBJ_QUEUE);
	zassert_not_null(q, "no memory for allocated queue object");
	k_queue_init(q);

	for (int i = 0; i < LIST_LEN * 2; i++) {
		qdata[i].data = i;
		zassert_false(k_queue_alloc_append(q, &qdata[i]), NULL);
	}

	for (int i = 0; i < LIST_LEN * 2; i++) {
		struct qdata *qd;

		qd = k_queue_get(q, K_NO_WAIT);
		zassert_true(qd != NULL, NULL);
		zassert_equal(qd->data, i, NULL);
	}
}

/**
 * @brief Test to verify free of allocated elements of queue
 * @ingroup kernel_queue_tests
 */
ZTEST(queue_api, test_auto_free)
{
	/* Ensure any resources requested by the previous test were released
	 * by allocating the entire pool. It would have allocated two kernel
	 * objects and five queue elements. The queue elements should be
	 * auto-freed when they are de-queued, and the objects when all
	 * threads with permissions exit.
	 */
	void *b[4];
	int i;

	if (!(IS_ENABLED(CONFIG_USERSPACE))) {
		ztest_test_skip();
	}

	for (i = 0; i < 4; i++) {
		b[i] = k_heap_alloc(&test_pool, 64, K_FOREVER);
		zassert_true(b[i] != NULL, "memory not auto released!");
	}

	for (i = 0; i < 4; i++) {
		k_heap_free(&test_pool, b[i]);
	}
}

#endif /* CONFIG_USERSPACE */
