/****************************************************************************
 * sched/pthread/pthread_mutex.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <sched.h>
#include <assert.h>
#include <errno.h>

#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/semaphore.h>

#include "sched/sched.h"
#include "pthread/pthread.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pthread_mutex_add
 *
 * Description:
 *   Add the mutex to the list of mutexes held by this pthread.
 *
 * Input Parameters:
 *  mutex - The mutex to be locked
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void pthread_mutex_add(FAR struct pthread_mutex_s *mutex)
{
  FAR struct tcb_s *rtcb;
  irqstate_t flags;

  DEBUGASSERT(mutex->flink == NULL);

  /* Add the mutex to the list of mutexes held by this pthread */

  flags        = enter_critical_section();
  rtcb         = this_task();
  mutex->flink = rtcb->mhead;
  rtcb->mhead  = mutex;
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: pthread_mutex_check
 *
 * Description:
 *   Verify that the mutex is not in the list of mutexes held by
 *   this pthread.
 *
 * Input Parameters:
 *  mutex - The mutex to be locked
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_DEBUG_ASSERTIONS
static void pthread_mutex_check(FAR struct pthread_mutex_s *mutex)
{
  FAR struct tcb_s *tcb = this_task();
  irqstate_t flags = enter_critical_section();
  FAR struct pthread_mutex_s *cur;

  DEBUGASSERT(mutex != NULL);
  for (cur = tcb->mhead; cur != NULL; cur = cur->flink)
    {
      /* The mutex should not be in the list of mutexes held by this task */

      DEBUGASSERT(cur != mutex);
    }

  leave_critical_section(flags);
}

#endif

/****************************************************************************
 * Name: pthread_mutex_remove
 *
 * Description:
 *   Remove the mutex to the list of mutexes held by this pthread.
 *
 * Input Parameters:
 *  mutex - The mutex to be locked
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void pthread_mutex_remove(FAR struct pthread_mutex_s *mutex)
{
  FAR struct tcb_s *rtcb;
  FAR struct pthread_mutex_s *curr;
  FAR struct pthread_mutex_s *prev;
  irqstate_t flags;

  flags = enter_critical_section();
  rtcb = this_task();

  /* Remove the mutex from the list of mutexes held by this task */

  for (prev = NULL, curr = rtcb->mhead;
       curr != NULL && curr != mutex;
       prev = curr, curr = curr->flink)
    {
    }

  DEBUGASSERT(curr == mutex);

  /* Remove the mutex from the list.  prev == NULL means that the mutex
   * to be removed is at the head of the list.
   */

  if (prev == NULL)
    {
      rtcb->mhead = mutex->flink;
    }
  else
    {
      prev->flink = mutex->flink;
    }

  mutex->flink = NULL;
  leave_critical_section(flags);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pthread_mutex_take
 *
 * Description:
 *   Take the pthread_mutex, waiting if necessary.  If successful, add the
 *   mutex to the list of mutexes held by this thread.
 *
 * Input Parameters:
 *  mutex - The mutex to be locked
 *
 * Returned Value:
 *   0 on success or an errno value on failure.
 *
 ****************************************************************************/

int pthread_mutex_take(FAR struct pthread_mutex_s *mutex,
                       FAR const struct timespec *abs_timeout)
{
  int ret = EINVAL;

  if (mutex != NULL)
    {
      /* Error out if the mutex is already in an inconsistent state. */

      if ((mutex->flags & _PTHREAD_MFLAGS_INCONSISTENT) != 0)
        {
          ret = EOWNERDEAD;
        }
      else
        {
          /* mutex_clocklock returns zero when successful, and the negative
           * errno value is returned when failed.
           */

          ret = -mutex_clocklock(&mutex->mutex, abs_timeout);
          if (ret == OK)
            {
              /* Check if the holder of the mutex has terminated without
               * releasing.  In that case, the state of the mutex is
               * inconsistent and we return EOWNERDEAD.
               */

              if ((mutex->flags & _PTHREAD_MFLAGS_INCONSISTENT) != 0)
                {
                  /* If the holder thread has terminated, we need to reset
                   * the mutex and return an error.
                   */

                  mutex_reset(&mutex->mutex);
                  ret = EOWNERDEAD;
                }

              /* If mutex is recursion, it is already in the linked list,
               * and we should not add it to the link list again.
               */

              else if (!mutex_is_recursive(&mutex->mutex))
                {
#ifdef CONFIG_DEBUG_ASSERTIONS
                  pthread_mutex_check(mutex);
#endif
                  pthread_mutex_add(mutex);
                }
            }
        }
    }

  return ret;
}

/****************************************************************************
 * Name: pthread_mutex_trytake
 *
 * Description:
 *   Try to take the pthread_mutex without waiting.  If successful, add the
 *   mutex to the list of mutexes held by this thread.
 *
 * Input Parameters:
 *  mutex - The mutex to be locked
 *  intr  - false: ignore EINTR errors when locking; true treat EINTR as
 *          other errors by returning the errno value
 *
 * Returned Value:
 *   0 on success or an errno value on failure.
 *
 ****************************************************************************/

int pthread_mutex_trytake(FAR struct pthread_mutex_s *mutex)
{
  int ret = EINVAL;

  /* Verify input parameters */

  DEBUGASSERT(mutex != NULL);
  if (mutex != NULL)
    {
      /* Error out if the mutex is already in an inconsistent state. */

      if ((mutex->flags & _PTHREAD_MFLAGS_INCONSISTENT) != 0)
        {
          ret = EOWNERDEAD;
        }
      else
        {
          /* Try to take the semaphore underlying the mutex */

          ret = mutex_trylock(&mutex->mutex);
          if (ret < 0)
            {
              ret = -ret;
            }
          else if (!mutex_is_recursive(&mutex->mutex))
            {
              /* If we successfully acquire the mutex, and we didn't get
               * it before, add the mutex to the linked list.
               */

              pthread_mutex_add(mutex);
            }
        }
    }

  return ret;
}

/****************************************************************************
 * Name: pthread_mutex_give
 *
 * Description:
 *   Take the pthread_mutex and, if successful, add the mutex to the list of
 *   mutexes held by this thread.
 *
 * Input Parameters:
 *  mutex - The mutex to be unlocked
 *
 * Returned Value:
 *   0 on success or an errno value on failure.
 *
 ****************************************************************************/

int pthread_mutex_give(FAR struct pthread_mutex_s *mutex)
{
  int ret = EINVAL;

  /* Verify input parameters */

  DEBUGASSERT(mutex != NULL);
  if (mutex != NULL)
    {
      /* Remove the mutex from the list of mutexes held by this task */

      if (!mutex_is_recursive(&mutex->mutex))
        {
          pthread_mutex_remove(mutex);
        }

      /* Now release the underlying mutex */

      ret = -mutex_unlock(&mutex->mutex);
    }

  return ret;
}

int pthread_mutex_breaklock(FAR struct pthread_mutex_s *mutex,
                            FAR unsigned int *breakval)
{
  int ret = EINVAL;

  /* Verify input parameters */

  DEBUGASSERT(mutex != NULL);
  if (mutex != NULL)
    {
      /* Remove the mutex from the list of mutexes held by this task */

      pthread_mutex_remove(mutex);

      /* Now release the underlying mutex */

      ret = -mutex_breaklock(&mutex->mutex, breakval);
    }

  return ret;
}

int pthread_mutex_restorelock(FAR struct pthread_mutex_s *mutex,
                              unsigned int breakval)
{
  int ret = EINVAL;

  /* Verify input parameters */

  DEBUGASSERT(mutex != NULL);
  if (mutex != NULL)
    {
      ret = -mutex_restorelock(&mutex->mutex, breakval);
      if (ret == OK)
        {
          /* Add the mutex to the list of mutexes held by this task */

          pthread_mutex_add(mutex);
        }
    }

  return ret;
}
