/*
 * Copyright © 2016 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
 * AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 */

#include "u_queue.h"

static PIPE_THREAD_ROUTINE(util_queue_thread_func, param)
{
   struct util_queue *queue = (struct util_queue*)param;

   while (1) {
      struct util_queue_job job;

      pipe_semaphore_wait(&queue->queued);
      if (queue->kill_thread)
         break;

      pipe_mutex_lock(queue->lock);
      job = queue->jobs[queue->read_idx];
      queue->jobs[queue->read_idx].job = NULL;
      queue->read_idx = (queue->read_idx + 1) % queue->max_jobs;
      pipe_mutex_unlock(queue->lock);

      pipe_semaphore_signal(&queue->has_space);

      if (job.job) {
         queue->execute_job(job.job);
         pipe_semaphore_signal(&job.fence->done);
      }
   }

   /* signal remaining jobs before terminating */
   pipe_mutex_lock(queue->lock);
   while (queue->jobs[queue->read_idx].job) {
      pipe_semaphore_signal(&queue->jobs[queue->read_idx].fence->done);
      queue->jobs[queue->read_idx].job = NULL;
      queue->read_idx = (queue->read_idx + 1) % queue->max_jobs;
   }
   pipe_mutex_unlock(queue->lock);
   return 0;
}

bool
util_queue_init(struct util_queue *queue,
                unsigned max_jobs,
                void (*execute_job)(void *))
{
   memset(queue, 0, sizeof(*queue));
   queue->max_jobs = max_jobs;

   queue->jobs = (struct util_queue_job*)
                 CALLOC(max_jobs, sizeof(struct util_queue_job));
   if (!queue->jobs)
      goto fail;

   queue->execute_job = execute_job;
   pipe_mutex_init(queue->lock);
   pipe_semaphore_init(&queue->has_space, max_jobs);
   pipe_semaphore_init(&queue->queued, 0);

   queue->thread = pipe_thread_create(util_queue_thread_func, queue);
   if (!queue->thread)
      goto fail;

   return true;

fail:
   if (queue->jobs) {
      pipe_semaphore_destroy(&queue->has_space);
      pipe_semaphore_destroy(&queue->queued);
      pipe_mutex_destroy(queue->lock);
      FREE(queue->jobs);
   }
   /* also util_queue_is_initialized can be used to check for success */
   memset(queue, 0, sizeof(*queue));
   return false;
}

void
util_queue_destroy(struct util_queue *queue)
{
   queue->kill_thread = 1;
   pipe_semaphore_signal(&queue->queued);
   pipe_thread_wait(queue->thread);
   pipe_semaphore_destroy(&queue->has_space);
   pipe_semaphore_destroy(&queue->queued);
   pipe_mutex_destroy(queue->lock);
   FREE(queue->jobs);
}

void
util_queue_fence_init(struct util_queue_fence *fence)
{
   pipe_semaphore_init(&fence->done, 1);
}

void
util_queue_fence_destroy(struct util_queue_fence *fence)
{
   pipe_semaphore_destroy(&fence->done);
}

void
util_queue_add_job(struct util_queue *queue,
                   void *job,
                   struct util_queue_fence *fence)
{
   struct util_queue_job *ptr;
   /* Set the semaphore to "busy". */
   pipe_semaphore_wait(&fence->done);

   /* if the queue is full, wait until there is space */
   pipe_semaphore_wait(&queue->has_space);

   pipe_mutex_lock(queue->lock);
   ptr = &queue->jobs[queue->write_idx];
   assert(ptr->job == NULL);
   ptr->job = job;
   ptr->fence = fence;
   queue->write_idx = (queue->write_idx + 1) % queue->max_jobs;
   pipe_mutex_unlock(queue->lock);
   pipe_semaphore_signal(&queue->queued);
}

void
util_queue_job_wait(struct util_queue_fence *fence)
{
   /* wait and set the semaphore to "busy" */
   pipe_semaphore_wait(&fence->done);
   /* set the semaphore to "idle" */
   pipe_semaphore_signal(&fence->done);
}
